#include "cvp.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>

Cvp::Reference::Reference(float inputSampleRate)
: Analyzer(inputSampleRate, 5.0f)
{
}

std::string Cvp::Reference::getIdentifier() const
{
    return "clapreference";
}

std::string Cvp::Reference::getName() const
{
    return "CLAP Reference";
}

std::string Cvp::Reference::getDescription() const
{
    return "Audio tagging and search by reference (query by example) using the "
           "audio embeddings of the CLAP and CLAP2 models of LAION. The references "
           "are regions of the audio stream provided as precomputed markers with a "
           "duration (for example the annotations of the host), the label of the "
           "marker being the class of the reference (the markers without duration "
           "define a region of the window size centered on the marker). The "
           "embeddings of the references of each class are averaged and the plugin "
           "generates over time the similarity scores between each audio window and "
           "each class as well as tags carrying the most similar class with its "
           "score (that can be used to filter the tags in the host). The classes can "
           "be organized hierarchically with slashes (for example, Animal/Bird/Owl), "
           "the references of a class being also used for its parent classes, and "
           "the tag is the most similar class among all the levels. With a single "
           "class, the similarity output finds the parts of the audio stream that "
           "sound like the references. The features are generated at the end of the "
           "analysis.";
}

Cvp::Reference::OutputList Cvp::Reference::getOutputDescriptors() const
{
    OutputDescriptor similarity;
    similarity.identifier = "similarity";
    similarity.name = "Class Scores";
    similarity.description = "The similarity scores between the audio window and the references of each class (the levels of the hierarchical classes are inserted before their sublevels)";
    similarity.unit = "";
    similarity.hasFixedBinCount = true;
    similarity.binNames = mHierarchy.getPaths();
    // As for the CLAP Prompt plugin, the output has at least two bins so that
    // its type (a matrix) does not depend on the number of classes, including
    // when the descriptors are requested before the references are set.
    similarity.binCount = std::max(similarity.binNames.size(), static_cast<size_t>(2));
    // The similarity scores are between -1 and 1 but the similarities between
    // audio embeddings are generally between 0.5 and 1: the extents are only
    // used to display the scores.
    similarity.hasKnownExtents = true;
    similarity.minValue = 0.5f;
    similarity.maxValue = 1.0f;
    similarity.isQuantized = false;
    similarity.sampleType = OutputDescriptor::SampleType::VariableSampleRate;
    similarity.hasDuration = true;

    OutputDescriptor tag;
    tag.identifier = "tag";
    tag.name = "Best Class";
    tag.description = "The most similar class of the audio window among all the levels of the hierarchical classes, with its similarity score";
    tag.unit = "";
    tag.hasFixedBinCount = true;
    tag.binCount = 0;
    tag.hasKnownExtents = false;
    tag.minValue = 0.0f;
    tag.maxValue = 0.0f;
    tag.isQuantized = false;
    tag.sampleType = OutputDescriptor::SampleType::VariableSampleRate;
    tag.hasDuration = true;
    return {similarity, tag};
}

Ive::PluginExtension::InputList Cvp::Reference::getInputDescriptors() const
{
    OutputDescriptor d;
    d.identifier = "reference";
    d.name = "Reference";
    d.description = "The audio regions used as references, one marker per region with the class as label (e.g. the annotations of the host), the levels of the hierarchical classes being separated by slashes (e.g. Bird/Blackbird and Bird/Robin)";
    d.unit = "";
    d.hasFixedBinCount = true;
    d.binCount = 0;
    d.hasKnownExtents = false;
    d.minValue = 0.0f;
    d.maxValue = 0.0f;
    d.isQuantized = false;
    d.sampleType = OutputDescriptor::SampleType::VariableSampleRate;
    d.hasDuration = true;
    return {d};
}

void Cvp::Reference::setPreComputingFeatures(FeatureSet const& fs)
{
    // The markers without label are references of the "Reference" class.
    auto const getLabel = [](Feature const& feature)
    {
        auto const label = Hierarchy::normalize(feature.label);
        return label.empty() ? std::string("Reference") : label;
    };
    std::vector<std::string> labels;
    auto const it = fs.find(0);
    if(it != fs.cend())
    {
        for(auto const& feature : it->second)
        {
            labels.push_back(getLabel(feature));
        }
    }
    mHierarchy = Hierarchy(labels);
    mRegions.clear();
    if(it != fs.cend())
    {
        for(auto const& feature : it->second)
        {
            auto const start = feature.timestamp.sec + static_cast<double>(feature.timestamp.nsec) / 1000000000.0;
            auto const duration = feature.hasDuration ? feature.duration.sec + static_cast<double>(feature.duration.nsec) / 1000000000.0 : 0.0;
            if(start >= 0.0)
            {
                mRegions.push_back({getLabel(feature), start, std::max(duration, 0.0)});
            }
        }
    }
    clear();
    invalidatePreparation();
}

Ive::PluginExtension::OutputExtraList Cvp::Reference::getOutputExtraDescriptors(size_t outputDescriptorIndex) const
{
    OutputExtraList list;
    if(outputDescriptorIndex == 1)
    {
        OutputExtraDescriptor d;
        d.identifier = "similarity";
        d.name = "Similarity";
        d.description = "The similarity score of the tagged class";
        d.unit = "";
        d.hasKnownExtents = true;
        d.minValue = 0.5f;
        d.maxValue = 1.0f;
        d.isQuantized = false;
        d.quantizeStep = 0.0f;
        list.push_back(std::move(d));
    }
    return list;
}

void Cvp::Reference::clear()
{
    mCaptures.clear();
    mSegments.clear();
    mCapturePosition = 0;
}

// Creates the captures of the audio of the regions. If there is no reference,
// the audio is ignored.
bool Cvp::Reference::prepareBeforeFirstBlock()
{
    if(mRegions.empty())
    {
        std::cerr << "No reference to analyze, the references must be provided as the precomputed markers of the plugin input\n";
        return false;
    }
    auto const sampleRate = static_cast<double>(getInputSampleRate());
    auto const window = getWindowSize();
    mCaptures.clear();
    for(auto const& region : mRegions)
    {
        Capture capture;
        capture.path = region.path;
        if(region.duration > 0.0)
        {
            capture.start = static_cast<size_t>(std::llround(region.start * sampleRate));
            capture.end = static_cast<size_t>(std::llround((region.start + region.duration) * sampleRate));
        }
        else
        {
            auto const center = static_cast<size_t>(std::llround(region.start * sampleRate));
            capture.start = center - std::min(center, window / 2);
            capture.end = center + (window - window / 2);
        }
        capture.end = std::max(capture.end, capture.start + 1);
        mCaptures.push_back(std::move(capture));
    }
    return true;
}

void Cvp::Reference::encode(Capture& capture)
{
    if(capture.chunk.empty())
    {
        return;
    }
    auto const embedding = getEncoder()->encodeAudio(capture.chunk.data(), capture.chunk.size(), static_cast<double>(getInputSampleRate()));
    if(!embedding.empty())
    {
        capture.sum.resize(embedding.size(), 0.0);
        for(size_t i = 0; i < embedding.size(); ++i)
        {
            capture.sum[i] += static_cast<double>(embedding[i]);
        }
        ++capture.numChunks;
    }
    capture.chunk.clear();
}

// The base class prepares the captures of the regions before the first block.
Cvp::Reference::FeatureSet Cvp::Reference::process(float const* const* inputBuffers, Vamp::RealTime timestamp)
{
    auto fs = Analyzer::process(inputBuffers, timestamp);
    captureRegions(inputBuffers[0], getBlockSize());
    return fs;
}

// The audio of the regions is encoded by chunks of window size while the audio
// stream is received, so that the memory does not depend on the duration of the regions.
void Cvp::Reference::captureRegions(float const* samples, size_t numSamples)
{
    auto const position = mCapturePosition;
    mCapturePosition += numSamples;
    auto const chunkSize = getWindowSize();
    for(auto& capture : mCaptures)
    {
        auto start = std::max(position, capture.start);
        auto const end = std::min(position + numSamples, capture.end);
        while(start < end)
        {
            auto const count = std::min(end - start, chunkSize - capture.chunk.size());
            capture.chunk.insert(capture.chunk.end(), samples + (start - position), samples + (start - position + count));
            start += count;
            if(capture.chunk.size() >= chunkSize)
            {
                encode(capture);
            }
        }
    }
}

void Cvp::Reference::processSegment([[maybe_unused]] size_t segment, Vamp::RealTime const& timestamp, Vamp::RealTime const& duration, std::vector<float> const& embedding, [[maybe_unused]] FeatureSet& fs)
{
    // The features are generated at the end because the references can be
    // anywhere in the audio stream.
    mSegments.push_back({timestamp, duration, embedding});
}

void Cvp::Reference::finish(FeatureSet& fs)
{
    auto const& paths = mHierarchy.getPaths();
    auto const chunkSize = getWindowSize();

    // The last chunk of a region is encoded if it is the only one or if it is
    // long enough to be representative (at least half the window size).
    std::vector<std::vector<float>> regionEmbeddings;
    for(auto& capture : mCaptures)
    {
        if(capture.numChunks == 0 || capture.chunk.size() >= chunkSize / 2)
        {
            encode(capture);
        }
        capture.chunk.clear();
        std::vector<float> embedding;
        if(capture.numChunks > 0)
        {
            embedding.reserve(capture.sum.size());
            for(auto const value : capture.sum)
            {
                embedding.push_back(static_cast<float>(value / static_cast<double>(capture.numChunks)));
            }
            normalize(embedding);
        }
        regionEmbeddings.push_back(std::move(embedding));
    }

    // The prototype of a class is the normalized average of the embeddings of
    // its references and of the references of its subclasses. The function returns
    // the sum of the embeddings of the references of the class and of its subclasses.
    std::vector<std::vector<float>> prototypes(paths.size());
    std::function<std::vector<double>(Hierarchy::Node const&)> average = [&](Hierarchy::Node const& node)
    {
        std::vector<double> sum;
        auto const add = [&](auto const& values)
        {
            sum.resize(values.size(), 0.0);
            std::transform(sum.cbegin(), sum.cend(), values.cbegin(), sum.begin(), std::plus<>());
        };
        for(size_t captureIndex = 0; captureIndex < mCaptures.size(); ++captureIndex)
        {
            auto const& embedding = regionEmbeddings.at(captureIndex);
            if(!embedding.empty() && mCaptures.at(captureIndex).path == node.path)
            {
                add(embedding);
            }
        }
        for(auto const& child : node.children)
        {
            auto const childSum = average(child);
            if(!childSum.empty())
            {
                add(childSum);
            }
        }
        if(sum.empty())
        {
            std::cerr << "No valid reference for the class " << node.path << " (the references must be inside the audio stream)\n";
        }
        else
        {
            auto& prototype = prototypes[node.index];
            prototype.resize(sum.size());
            std::transform(sum.cbegin(), sum.cend(), prototype.begin(), [](double value)
                           {
                               return static_cast<float>(value);
                           });
            normalize(prototype);
        }
        return sum;
    };
    for(auto const& root : mHierarchy.getRoots())
    {
        average(root);
    }

    for(auto const& segment : mSegments)
    {
        if(segment.embedding.empty())
        {
            continue;
        }
        // The classes without reference are ignored by the tags and their
        // similarity scores are zero.
        std::vector<float> scores;
        scores.reserve(prototypes.size());
        for(auto const& prototype : prototypes)
        {
            scores.push_back(prototype.empty() ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(dot(segment.embedding, prototype)));
        }
        auto const selected = findBest(scores);

        Feature similarity;
        similarity.hasTimestamp = true;
        similarity.timestamp = segment.timestamp;
        similarity.hasDuration = true;
        similarity.duration = segment.duration;
        similarity.values.reserve(scores.size());
        for(auto const score : scores)
        {
            similarity.values.push_back(std::isfinite(score) ? score : 0.0f);
        }
        // The output has at least two bins.
        similarity.values.resize(std::max(similarity.values.size(), static_cast<size_t>(2)), 0.0f);
        fs[0].push_back(std::move(similarity));

        if(selected.has_value())
        {
            Feature tag;
            tag.hasTimestamp = true;
            tag.timestamp = segment.timestamp;
            tag.hasDuration = true;
            tag.duration = segment.duration;
            tag.label = paths.at(*selected);
            tag.values.push_back(scores.at(*selected));
            fs[1].push_back(std::move(tag));
        }
    }
    mSegments.clear();
}
