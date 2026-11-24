#include "cvp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vamp-sdk/PluginAdapter.h>

// Analyzer

Cvp::Analyzer::Analyzer(float inputSampleRate, float defaultWindowSeconds)
: Vamp::Plugin(inputSampleRate)
, mDefaultWindowSeconds(defaultWindowSeconds)
, mModelPaths(getModelPaths())
, mModelIndex(getDefaultModelIndex())
, mWindowSeconds(static_cast<double>(defaultWindowSeconds))
{
}

bool Cvp::Analyzer::initialise(size_t channels, size_t stepSize, size_t blockSize)
{
    if(channels != static_cast<size_t>(1) || stepSize != blockSize || blockSize == 0)
    {
        return false;
    }
    // The model is only reloaded if it changed since the previous initialization.
    if(mEncoder == nullptr || mEncoderModelIndex != mModelIndex)
    {
        mEncoder.reset();
        if(mModelIndex < mModelPaths.size())
        {
            mEncoder = Encoder::create(mModelPaths.at(mModelIndex));
        }
        else
        {
            std::cerr << "Invalid model index : " << mModelIndex << "\n";
        }
        mEncoderModelIndex = mModelIndex;
    }
    mBlockSize = blockSize;
    reset();
    return mEncoder != nullptr;
}

Cvp::Analyzer::InputDomain Cvp::Analyzer::getInputDomain() const
{
    return TimeDomain;
}

std::string Cvp::Analyzer::getMaker() const
{
    return "Ircam";
}

int Cvp::Analyzer::getPluginVersion() const
{
    return PLUGIN_VERSION;
}

std::string Cvp::Analyzer::getCopyright() const
{
    return "CLAP and CLAP2 models by LAION. CLAP.cpp by Gorse. Clap Vamp Plugin by Pierre Guillot. Copyright 2026 Ircam. MIT License.";
}

size_t Cvp::Analyzer::getPreferredBlockSize() const
{
    return static_cast<size_t>(1024);
}

size_t Cvp::Analyzer::getPreferredStepSize() const
{
    return static_cast<size_t>(0);
}

Cvp::Analyzer::ParameterList Cvp::Analyzer::getParameterDescriptors() const
{
    ParameterList list;
    {
        auto const& models = mModelPaths;
        ParameterDescriptor param;
        param.identifier = "model";
        param.name = "Model";
        param.description = "The model used to generate the embeddings (the embedded models are listed first, followed by the models of the directory defined by the CLAPMODELSPATH environment variable)";
        param.unit = "";
        // The models are numbered from 1 in the order of their index.
        for(size_t index = 0; index < models.size(); ++index)
        {
            param.valueNames.push_back(std::to_string(index + 1) + ". " + getModelDisplayName(models.at(index)));
        }
        param.minValue = 0.0f;
        param.maxValue = static_cast<float>(std::max(models.size(), static_cast<size_t>(1)) - 1);
        param.defaultValue = static_cast<float>(getDefaultModelIndex());
        param.isQuantized = true;
        param.quantizeStep = 1.0f;
        list.push_back(std::move(param));
    }
    {
        ParameterDescriptor param;
        param.identifier = "windowsize";
        param.name = "Window Size";
        param.description = "The duration of the analysis windows, in seconds";
        param.unit = "s";
        param.minValue = 1.0f;
        param.maxValue = 10.0f;
        param.defaultValue = mDefaultWindowSeconds;
        param.isQuantized = true;
        param.quantizeStep = 0.5f;
        list.push_back(std::move(param));
    }
    {
        ParameterDescriptor param;
        param.identifier = "hopsize";
        param.name = "Hop Size";
        param.description = "The duration between two analysis windows, in seconds (the computation time is proportional to the number of windows)";
        param.unit = "s";
        param.minValue = 0.1f;
        param.maxValue = 10.0f;
        param.defaultValue = 1.0f;
        param.isQuantized = true;
        param.quantizeStep = 0.1f;
        list.push_back(std::move(param));
    }
    return list;
}

void Cvp::Analyzer::setParameter(std::string paramid, float newval)
{
    // The values are quantized to the steps of the descriptors.
    if(paramid == "model")
    {
        auto const max = static_cast<float>(std::max(mModelPaths.size(), static_cast<size_t>(1)) - 1);
        mModelIndex = static_cast<size_t>(std::lround(std::clamp(newval, 0.0f, max)));
    }
    else if(paramid == "windowsize")
    {
        mWindowSeconds = std::round(std::clamp(static_cast<double>(newval), 1.0, 10.0) * 2.0) / 2.0;
    }
    else if(paramid == "hopsize")
    {
        mHopSeconds = std::round(std::clamp(static_cast<double>(newval), 0.1, 10.0) * 10.0) / 10.0;
    }
    else
    {
        std::cerr << "Invalid parameter : " << paramid << "\n";
    }
}

float Cvp::Analyzer::getParameter(std::string paramid) const
{
    if(paramid == "model")
    {
        return static_cast<float>(mModelIndex);
    }
    if(paramid == "windowsize")
    {
        return static_cast<float>(mWindowSeconds);
    }
    if(paramid == "hopsize")
    {
        return static_cast<float>(mHopSeconds);
    }
    std::cerr << "Invalid parameter : " << paramid << "\n";
    return 0.0f;
}

Cvp::Encoder* Cvp::Analyzer::getEncoder() const
{
    return mEncoder.get();
}

size_t Cvp::Analyzer::getBlockSize() const
{
    return mBlockSize;
}

size_t Cvp::Analyzer::getWindowSize() const
{
    return std::max(static_cast<size_t>(1), static_cast<size_t>(std::lround(mWindowSeconds * static_cast<double>(getInputSampleRate()))));
}

size_t Cvp::Analyzer::getHopSize() const
{
    return std::max(static_cast<size_t>(1), static_cast<size_t>(std::lround(mHopSeconds * static_cast<double>(getInputSampleRate()))));
}

Vamp::RealTime Cvp::Analyzer::toRealTime(size_t samples, double sampleRate)
{
    return Vamp::RealTime::fromSeconds(static_cast<double>(samples) / sampleRate);
}

void Cvp::Analyzer::reset()
{
    mBuffer.clear();
    mBuffer.reserve(getWindowSize() + mBlockSize);
    mBufferPosition = 0;
    mReceivedSamples = 0;
    mNextSegment = 0;
    mPrepared.reset();
    clear();
}

void Cvp::Analyzer::finish([[maybe_unused]] FeatureSet& fs)
{
}

bool Cvp::Analyzer::prepareBeforeFirstBlock()
{
    return true;
}

bool Cvp::Analyzer::isPrepared()
{
    if(!mPrepared.has_value())
    {
        mPrepared = mEncoder != nullptr && prepareBeforeFirstBlock();
    }
    return *mPrepared;
}

void Cvp::Analyzer::invalidatePreparation()
{
    if(mPrepared.has_value())
    {
        mPrepared = false;
    }
}

void Cvp::Analyzer::clear()
{
}

// The segment k starts at k * hop and its window is centered on the segment.
// The positions are signed because the windows of the first segments start
// before the beginning of the audio stream.
namespace
{
    struct Window
    {
        int64_t start;
        int64_t end;
    };

    Window getWindow(size_t segment, size_t hop, size_t window)
    {
        auto const center = static_cast<int64_t>(segment * hop + hop / 2);
        auto const start = center - static_cast<int64_t>(window / 2);
        return {start, start + static_cast<int64_t>(window)};
    }
} // namespace

size_t Cvp::Analyzer::getReceivedSamples() const
{
    return mReceivedSamples;
}

// Analyzes the next segment with the audio received before endPosition (the
// window is shortened if it exceeds the beginning or endPosition).
void Cvp::Analyzer::analyse(size_t endPosition, FeatureSet& fs)
{
    auto const sampleRate = static_cast<double>(getInputSampleRate());
    auto const hop = getHopSize();
    auto const window = getWindow(mNextSegment, hop, getWindowSize());
    auto start = static_cast<size_t>(std::max(window.start, static_cast<int64_t>(0)));
    auto end = std::min(static_cast<size_t>(std::max(window.end, static_cast<int64_t>(0))), endPosition);
    auto const segmentPosition = mNextSegment * hop;
    // If the window of the last segment starts after the end of the audio stream
    // (when the window is shorter than the hop), the window analyzes the end of
    // the audio stream instead.
    if(end <= start)
    {
        end = endPosition;
        start = std::max(mBufferPosition, endPosition - std::min(endPosition, getWindowSize()));
    }

    std::vector<float> embedding;
    if(start >= mBufferPosition && end > start && end - mBufferPosition <= mBuffer.size())
    {
        embedding = getEncoder()->encodeAudio(mBuffer.data() + (start - mBufferPosition), end - start, sampleRate);
    }
    auto const timestamp = toRealTime(segmentPosition, sampleRate);
    auto const duration = toRealTime(std::min(hop, mReceivedSamples - segmentPosition), sampleRate);
    processSegment(mNextSegment, timestamp, duration, embedding, fs);
    ++mNextSegment;

    // The samples before the window of the next segment (and before the next
    // segment itself) are no longer necessary.
    auto const nextWindowStart = static_cast<size_t>(std::max(getWindow(mNextSegment, hop, getWindowSize()).start, static_cast<int64_t>(0)));
    auto const nextStart = std::min(nextWindowStart, mNextSegment * hop);
    if(nextStart > mBufferPosition)
    {
        auto const count = std::min(nextStart - mBufferPosition, mBuffer.size());
        mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(count));
        mBufferPosition += count;
    }
}

Cvp::Analyzer::FeatureSet Cvp::Analyzer::process(float const* const* inputBuffers, [[maybe_unused]] Vamp::RealTime timestamp)
{
    FeatureSet fs;
    if(!isPrepared())
    {
        return fs;
    }
    auto const* inputBuffer = inputBuffers[0];
    // The audio is appended as is: clap_encode_audio resamples it.
    mBuffer.insert(mBuffer.end(), inputBuffer, inputBuffer + mBlockSize);
    mReceivedSamples += mBlockSize;
    auto const hop = getHopSize();
    auto const window = getWindowSize();
    // A segment is analyzed once its window and the segment itself are received
    // (the window can be shorter than the segment).
    while(std::max(getWindow(mNextSegment, hop, window).end, static_cast<int64_t>((mNextSegment + 1) * hop)) <= static_cast<int64_t>(mReceivedSamples))
    {
        analyse(mReceivedSamples, fs);
    }
    return fs;
}

Cvp::Analyzer::FeatureSet Cvp::Analyzer::getRemainingFeatures()
{
    FeatureSet fs;
    if(!isPrepared())
    {
        return fs;
    }
    auto const hop = getHopSize();
    while(mNextSegment * hop < mReceivedSamples)
    {
        analyse(mReceivedSamples, fs);
    }
    finish(fs);
    return fs;
}

#ifdef __cplusplus
extern "C"
{
#endif
    VampPluginDescriptor const* vampGetPluginDescriptor(unsigned int version, unsigned int index)
    {
        if(version < 1)
        {
            return nullptr;
        }
        switch(index)
        {
            case 0:
            {
                static Vamp::PluginAdapter<Cvp::Prompt> adaptater;
                return adaptater.getDescriptor();
            }
            case 1:
            {
                static Vamp::PluginAdapter<Cvp::Reference> adaptater;
                return adaptater.getDescriptor();
            }
            case 2:
            {
                static Vamp::PluginAdapter<Cvp::Novelty> adaptater;
                return adaptater.getDescriptor();
            }
            default:
            {
                return nullptr;
            }
        }
    }

    IVE_EXTERN IvePluginDescriptor const* iveGetPluginDescriptor(unsigned int version, unsigned int index)
    {
        if(version < 1)
        {
            return nullptr;
        }
        switch(index)
        {
            case 0:
            {
                return Ive::PluginAdapter::getDescriptor<Cvp::Prompt>();
            }
            case 1:
            {
                return Ive::PluginAdapter::getDescriptor<Cvp::Reference>();
            }
            case 2:
            {
                return Ive::PluginAdapter::getDescriptor<Cvp::Novelty>();
            }
            default:
            {
                return nullptr;
            }
        }
    }
#ifdef __cplusplus
}
#endif
