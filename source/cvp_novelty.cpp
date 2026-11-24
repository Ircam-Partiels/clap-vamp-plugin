#include "cvp.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

Cvp::Novelty::Novelty(float inputSampleRate)
: Analyzer(inputSampleRate, 5.0f)
{
}

std::string Cvp::Novelty::getIdentifier() const
{
    return "clapnovelty";
}

std::string Cvp::Novelty::getName() const
{
    return "CLAP Novelty";
}

std::string Cvp::Novelty::getDescription() const
{
    return "Detection of the changes in the audio stream using the audio "
           "embeddings of the CLAP and CLAP2 models of LAION, without text prompt "
           "nor reference. At each boundary between two analysis windows, the plugin "
           "compares the average embedding of the windows entirely before the "
           "boundary with the average embedding of the windows entirely after the "
           "boundary, over the duration of the Context parameter. The windows "
           "shortened at the beginning and at the end of the audio stream are "
           "ignored, hence the novelty is zero (no change is considered) during "
           "approximately the window size at the beginning and at the end of the "
           "audio stream. "
           "The novelty is the cosine "
           "distance between the two embeddings, between 0 (the content does not "
           "change) and 2 (in practice, the audio embeddings remain similar and the "
           "novelty rarely exceeds 0.5). The boundaries are the peaks of the novelty, separated by at "
           "least the context duration, with the novelty as value (the boundaries "
           "can be filtered by the host according to this value). The features are "
           "generated at the end of the analysis.";
}

Cvp::Novelty::OutputList Cvp::Novelty::getOutputDescriptors() const
{
    OutputDescriptor novelty;
    novelty.identifier = "novelty";
    novelty.name = "Novelty";
    novelty.description = "The novelty at each boundary between two analysis windows (the cosine distance between the average embeddings before and after the boundary, zero at the beginning and at the end of the audio stream)";
    novelty.unit = "";
    novelty.hasFixedBinCount = true;
    novelty.binCount = 1;
    // The novelty is between 0 and 2 but generally lower than 0.3: the extents
    // are only used to display the novelty.
    novelty.hasKnownExtents = true;
    novelty.minValue = 0.0f;
    novelty.maxValue = 0.3f;
    novelty.isQuantized = false;
    novelty.sampleType = OutputDescriptor::SampleType::VariableSampleRate;
    novelty.hasDuration = false;

    OutputDescriptor boundary;
    boundary.identifier = "boundary";
    boundary.name = "Boundary";
    boundary.description = "The boundaries where the content of the audio changes (the peaks of the novelty)";
    boundary.unit = "";
    boundary.hasFixedBinCount = true;
    boundary.binCount = 0;
    boundary.hasKnownExtents = false;
    boundary.isQuantized = false;
    boundary.sampleType = OutputDescriptor::SampleType::VariableSampleRate;
    boundary.hasDuration = false;
    return {novelty, boundary};
}

Cvp::Novelty::ParameterList Cvp::Novelty::getParameterDescriptors() const
{
    auto list = Analyzer::getParameterDescriptors();
    {
        ParameterDescriptor param;
        param.identifier = "context";
        param.name = "Context";
        param.description = "The duration compared before and after each boundary and the minimum duration between two detected boundaries, in seconds";
        param.unit = "s";
        param.minValue = 0.1f;
        param.maxValue = 60.0f;
        param.defaultValue = 5.0f;
        param.isQuantized = true;
        param.quantizeStep = 0.1f;
        list.push_back(std::move(param));
    }
    return list;
}

void Cvp::Novelty::setParameter(std::string paramid, float newval)
{
    if(paramid == "context")
    {
        mContextSeconds = std::round(std::clamp(static_cast<double>(newval), 0.1, 60.0) * 10.0) / 10.0;
    }
    else
    {
        Analyzer::setParameter(paramid, newval);
    }
}

float Cvp::Novelty::getParameter(std::string paramid) const
{
    if(paramid == "context")
    {
        return static_cast<float>(mContextSeconds);
    }
    return Analyzer::getParameter(paramid);
}

void Cvp::Novelty::setPreComputingFeatures([[maybe_unused]] FeatureSet const& fs)
{
}

Ive::PluginExtension::OutputExtraList Cvp::Novelty::getOutputExtraDescriptors(size_t outputDescriptorIndex) const
{
    OutputExtraList list;
    if(outputDescriptorIndex == 1)
    {
        OutputExtraDescriptor d;
        d.identifier = "novelty";
        d.name = "Novelty";
        d.description = "The novelty of the boundary";
        d.unit = "";
        d.hasKnownExtents = true;
        d.minValue = 0.0f;
        d.maxValue = 0.3f;
        d.isQuantized = false;
        d.quantizeStep = 0.0f;
        list.push_back(std::move(d));
    }
    return list;
}

void Cvp::Novelty::clear()
{
    mEmbeddings.clear();
    mTimestamps.clear();
}

void Cvp::Novelty::processSegment([[maybe_unused]] size_t segment, Vamp::RealTime const& timestamp, [[maybe_unused]] Vamp::RealTime const& duration, std::vector<float> const& embedding, [[maybe_unused]] FeatureSet& fs)
{
    // The features are generated at the end because the novelty depends on the following windows.
    mEmbeddings.push_back(embedding);
    mTimestamps.push_back(timestamp);
}

void Cvp::Novelty::finish(FeatureSet& fs)
{
    // The shortened windows at the beginning and at the end of the audio stream
    // are ignored because their embeddings are not comparable with the others.
    // As in the analyzer, the window of the segment j starts at
    // j * hop + hop / 2 - window / 2.
    {
        auto const hop = getHopSize();
        auto const window = getWindowSize();
        auto const receivedSamples = getReceivedSamples();
        for(size_t segment = 0; segment < mEmbeddings.size(); ++segment)
        {
            auto const center = segment * hop + hop / 2;
            if(center < window / 2 || center - window / 2 + window > receivedSamples)
            {
                mEmbeddings[segment].clear();
            }
        }
    }
    auto const numSegments = mEmbeddings.size();
    auto const hopSeconds = static_cast<double>(getHopSize()) / static_cast<double>(getInputSampleRate());
    auto const context = std::max(static_cast<size_t>(1), static_cast<size_t>(std::lround(mContextSeconds / hopSeconds)));

    // Returns the normalized average of the valid embeddings of the segments [start, end[
    auto const average = [&](size_t start, size_t end)
    {
        std::vector<float> result;
        std::vector<double> sum;
        for(auto index = start; index < end; ++index)
        {
            auto const& embedding = mEmbeddings.at(index);
            if(!embedding.empty())
            {
                sum.resize(embedding.size(), 0.0);
                for(size_t i = 0; i < embedding.size(); ++i)
                {
                    sum[i] += static_cast<double>(embedding[i]);
                }
            }
        }
        result.reserve(sum.size());
        for(auto const value : sum)
        {
            result.push_back(static_cast<float>(value));
        }
        normalize(result);
        return result;
    };

    // The windows are centered on the segments and can be longer than the hop
    // size: the windows that overlap the boundary are skipped so that only the
    // windows entirely before the boundary are compared with the windows
    // entirely after the boundary. The window of the segment j covers
    // [(j + 0.5) * hop - window / 2, (j + 0.5) * hop + window / 2[.
    auto const halfWindowInHops = static_cast<double>(getWindowSize()) / (2.0 * static_cast<double>(getHopSize()));
    auto const skip = static_cast<size_t>(std::max(0.0, std::ceil(halfWindowInHops - 0.5 - 1.0e-9)));

    // The novelty of the boundary k (the start of the segment k) compares the
    // segments [k - skip - context, k - skip[ with the segments [k + skip, k + skip + context[.
    std::vector<float> novelties(numSegments, std::numeric_limits<float>::quiet_NaN());
    for(size_t index = 1; index < numSegments; ++index)
    {
        if(index < skip + 1 || index + skip >= numSegments)
        {
            continue;
        }
        auto const beforeEnd = index - skip;
        auto const before = average(beforeEnd - std::min(beforeEnd, context), beforeEnd);
        auto const after = average(index + skip, std::min(numSegments, index + skip + context));
        if(before.empty() || after.empty())
        {
            continue;
        }
        // The cosine distance of two normalized embeddings is between 0 and 2,
        // the rounding errors can give a slightly negative value for identical
        // embeddings.
        novelties[index] = static_cast<float>(std::clamp(1.0 - dot(before, after), 0.0, 2.0));
    }

    // The boundaries without complete window before or after them (at the
    // beginning and at the end of the audio stream) are considered unchanged:
    // their novelty is zero but they cannot be detected as boundaries.
    for(size_t index = 0; index < numSegments; ++index)
    {
        Feature feature;
        feature.hasTimestamp = true;
        feature.timestamp = mTimestamps.at(index);
        feature.hasDuration = false;
        feature.values.push_back(std::isfinite(novelties[index]) ? novelties[index] : 0.0f);
        fs[0].push_back(std::move(feature));
    }

    // A boundary is a peak of the novelty, the highest value in the context
    // around it (the first one if several values are equal). The novelties
    // below the rounding errors of the embeddings are considered unchanged
    // (for example, with silence or a constant signal), hence they cannot
    // be boundaries.
    auto constexpr minNovelty = 1.0e-6f;
    for(size_t index = 1; index < numSegments; ++index)
    {
        auto const value = novelties[index];
        if(!std::isfinite(value) || value < minNovelty)
        {
            continue;
        }
        auto isPeak = true;
        auto const first = index - std::min(index, context);
        auto const last = std::min(numSegments - 1, index + context);
        for(auto other = first; other <= last && isPeak; ++other)
        {
            auto const otherValue = novelties[other];
            if(other != index && std::isfinite(otherValue))
            {
                isPeak = other < index ? value > otherValue : value >= otherValue;
            }
        }
        if(isPeak)
        {
            Feature feature;
            feature.hasTimestamp = true;
            feature.timestamp = mTimestamps.at(index);
            feature.hasDuration = false;
            // The label is formatted with the classic locale (std::to_string
            // depends on the locale of the host, that can use a decimal comma).
            std::ostringstream label;
            label.imbue(std::locale::classic());
            label << std::fixed << std::setprecision(6) << value;
            feature.label = label.str();
            feature.values.push_back(value);
            fs[1].push_back(std::move(feature));
        }
    }
    clear();
}
