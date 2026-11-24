// Tests the analysis of the plugins with a model (the Vamp plugin tester cannot
// provide the prompts and the examples that are precomputed features of the
// Ircam Vamp Extension). The models are found with the CLAPMODELSPATH
// environment variable (the test is not in the Vamp bundle). The tests check
// - the outputs and the timestamps of the features of the CLAP Prompt plugin with a
//   block size smaller than the hop size (but not a divisor) and a block size
//   larger than several hop sizes, with a CLAP and a CLAP2 model,
// - the hierarchical prompts,
// - the tags of the CLAP Example plugin on a signal made of a tone and a noise,
// - the boundary detected by the CLAP Novelty plugin between the tone and the noise.

#include "cvp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
    int gFailures = 0;
    auto constexpr gSampleRate = 44100.0f;
    auto constexpr gPi = 3.14159265358979323846;

    void check(bool condition, std::string const& message)
    {
        if(!condition)
        {
            std::cerr << "FAILED: " << message << "\n";
            ++gFailures;
        }
    }

    double toSeconds(Vamp::RealTime const& time)
    {
        return static_cast<double>(time.sec) + static_cast<double>(time.nsec) / 1000000000.0;
    }

    // Selects the model by name (the last model with the name is the one of the
    // CLAPMODELSPATH directory).
    bool selectModel(Vamp::Plugin& plugin, std::string const& modelName)
    {
        auto const parameters = plugin.getParameterDescriptors();
        auto const modelParam = std::find_if(parameters.cbegin(), parameters.cend(), [](auto const& param)
                                             {
                                                 return param.identifier == "model";
                                             });
        check(modelParam != parameters.cend(), "the model parameter exists");
        if(modelParam == parameters.cend())
        {
            return false;
        }
        // The bundled models that are not in the bundle are listed first, hence
        // the last model with this name is selected (in the CLAPMODELSPATH directory).
        auto const paths = Cvp::getModelPaths();
        check(modelParam->valueNames.size() == paths.size(), "the model parameter lists all the models");
        auto const path = std::find_if(paths.crbegin(), paths.crend(), [&](auto const& modelPath)
                                       {
                                           return Cvp::toUtf8(modelPath.stem()) == modelName && std::filesystem::exists(modelPath);
                                       });
        check(path != paths.crend(), "the model " + modelName + " is available (check the CLAPMODELSPATH environment variable)");
        if(path == paths.crend())
        {
            return false;
        }
        auto const modelIndex = paths.size() - 1 - static_cast<size_t>(std::distance(paths.crbegin(), path));
        plugin.setParameter("model", static_cast<float>(modelIndex));
        return true;
    }

    Vamp::Plugin::FeatureSet makeMarkers(std::vector<std::string> const& labels)
    {
        Vamp::Plugin::FeatureSet input;
        for(auto const& label : labels)
        {
            Vamp::Plugin::Feature feature;
            feature.hasTimestamp = true;
            feature.label = label;
            input[0].push_back(feature);
        }
        return input;
    }

    // Runs the plugin on the signal (the last block is padded with zeros as the
    // hosts do). As in Partiels, the precomputed features are set after the initialization.
    Vamp::Plugin::FeatureSet run(Cvp::Analyzer& plugin, std::function<float(size_t)> const& signal, size_t numFrames, size_t blockSize, Vamp::Plugin::FeatureSet const& input)
    {
        Vamp::Plugin::FeatureSet output;
        check(plugin.initialise(1, blockSize, blockSize), "the plugin " + plugin.getIdentifier() + " is initialized");
        plugin.setPreComputingFeatures(input);
        auto const numBlocks = (numFrames + blockSize - 1) / blockSize;
        std::vector<float> block(blockSize);
        auto const append = [&](Vamp::Plugin::FeatureSet const& fs)
        {
            for(auto const& [index, features] : fs)
            {
                auto& list = output[index];
                list.insert(list.end(), features.cbegin(), features.cend());
            }
        };
        for(size_t blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
        {
            for(size_t i = 0; i < blockSize; ++i)
            {
                auto const frame = blockIndex * blockSize + i;
                block[i] = frame < numFrames ? signal(frame) : 0.0f;
            }
            float const* buffers[] = {block.data()};
            auto const timestamp = Vamp::RealTime::fromSeconds(static_cast<double>(blockIndex * blockSize) / static_cast<double>(gSampleRate));
            append(plugin.process(buffers, timestamp));
        }
        append(plugin.getRemainingFeatures());
        return output;
    }

    float sine(size_t frame)
    {
        return static_cast<float>(0.5 * std::sin(2.0 * gPi * 440.0 * static_cast<double>(frame) / static_cast<double>(gSampleRate)));
    }

    // A deterministic white noise
    float noise(size_t frame)
    {
        auto state = static_cast<uint32_t>(frame) * 747796405u + 2891336453u;
        state = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        state = (state >> 22u) ^ state;
        return static_cast<float>(static_cast<double>(state) / 4294967295.0 - 0.5);
    }

    // A tone during the first half of the signal and a noise during the second half
    float toneThenNoise(size_t frame, size_t numFrames)
    {
        return frame < numFrames / 2 ? sine(frame) : noise(frame);
    }

    // Checks the timestamps and durations of the windows
    void checkWindows(std::vector<Vamp::Plugin::Feature> const& features, size_t processedFrames, size_t hopFrames, size_t numValues)
    {
        auto const expectedCount = (processedFrames + hopFrames - 1) / hopFrames;
        check(features.size() == expectedCount, "the number of similarity features is " + std::to_string(features.size()) + " instead of " + std::to_string(expectedCount));
        auto constexpr epsilon = 1.0e-6;
        auto totalDuration = 0.0;
        for(size_t i = 0; i < features.size(); ++i)
        {
            auto const& feature = features.at(i);
            auto const expectedTime = static_cast<double>(i * hopFrames) / static_cast<double>(gSampleRate);
            auto const expectedDuration = static_cast<double>(std::min(hopFrames, processedFrames - i * hopFrames)) / static_cast<double>(gSampleRate);
            check(feature.hasTimestamp && std::abs(toSeconds(feature.timestamp) - expectedTime) < epsilon, "the similarity " + std::to_string(i) + " starts at " + std::to_string(toSeconds(feature.timestamp)) + "s instead of " + std::to_string(expectedTime) + "s");
            check(feature.hasDuration && std::abs(toSeconds(feature.duration) - expectedDuration) < epsilon, "the similarity " + std::to_string(i) + " lasts " + std::to_string(toSeconds(feature.duration)) + "s instead of " + std::to_string(expectedDuration) + "s");
            check(feature.values.size() == numValues, "the similarity " + std::to_string(i) + " has one value per prompt");
            check(std::all_of(feature.values.cbegin(), feature.values.cend(), [](auto const value)
                              {
                                  return std::isfinite(value) && value >= -1.001f && value <= 1.001f;
                              }),
                  "the similarity scores are in the range [-1, 1]");
            check(feature.label.empty(), "the similarity features have no label");
            totalDuration += toSeconds(feature.duration);
        }
        check(std::abs(totalDuration - static_cast<double>(processedFrames) / static_cast<double>(gSampleRate)) < 1.0e-4, "the similarity features cover the whole signal");
    }

    void checkTags(std::vector<Vamp::Plugin::Feature> const& tags, std::vector<Vamp::Plugin::Feature> const& similarities, std::vector<std::string> const& paths)
    {
        check(tags.size() == similarities.size(), "there is one tag per window");
        for(auto const& tag : tags)
        {
            check(std::find(paths.cbegin(), paths.cend(), tag.label) != paths.cend(), "the tag " + tag.label + " is one of the classes");
            check(!tag.values.empty(), "the tag has the similarity score");
            auto const similarity = std::find_if(similarities.cbegin(), similarities.cend(), [&](auto const& feature)
                                                 {
                                                     return feature.timestamp == tag.timestamp && feature.duration == tag.duration;
                                                 });
            check(similarity != similarities.cend(), "the tag matches an analysis window");
            if(similarity != similarities.cend() && !tag.values.empty())
            {
                auto const index = static_cast<size_t>(std::distance(paths.cbegin(), std::find(paths.cbegin(), paths.cend(), tag.label)));
                check(index < similarity->values.size() && similarity->values.at(index) == tag.values.at(0), "the tag score is the similarity score of the class");
                check(tag.values.at(0) == *std::max_element(similarity->values.cbegin(), similarity->values.cend()), "the tag is the class with the highest score");
            }
        }
    }

    // Checks that the probabilities of the siblings sum to the probability of
    // their parent (or to 1 for the top-level prompts) and that the tags carry
    // the probability of their class
    void checkProbabilities(Cvp::Hierarchy const& hierarchy, std::vector<Vamp::Plugin::Feature> const& probabilities, std::vector<Vamp::Plugin::Feature> const& similarities, std::vector<Vamp::Plugin::Feature> const& tags)
    {
        check(probabilities.size() == similarities.size(), "there is one probability feature per window");
        auto const& paths = hierarchy.getPaths();
        for(size_t i = 0; i < probabilities.size() && i < similarities.size(); ++i)
        {
            auto const& values = probabilities.at(i).values;
            check(probabilities.at(i).timestamp == similarities.at(i).timestamp && probabilities.at(i).duration == similarities.at(i).duration, "the probabilities match the analysis windows");
            check(values.size() == similarities.at(i).values.size(), "the probability feature has one value per prompt");
            if(values.size() < paths.size())
            {
                continue;
            }
            check(std::all_of(values.cbegin(), values.cend(), [](auto const value)
                              {
                                  return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
                              }),
                  "the probabilities are in the range [0, 1]");
            std::function<void(std::vector<Cvp::Hierarchy::Node> const&, double)> checkSiblings = [&](std::vector<Cvp::Hierarchy::Node> const& nodes, double expected)
            {
                if(nodes.empty())
                {
                    return;
                }
                auto sum = 0.0;
                for(auto const& node : nodes)
                {
                    sum += static_cast<double>(values.at(node.index));
                    checkSiblings(node.children, static_cast<double>(values.at(node.index)));
                }
                check(std::abs(sum - expected) < 1.0e-4, "the probabilities of the siblings sum to the probability of their parent (" + std::to_string(sum) + " instead of " + std::to_string(expected) + ")");
            };
            checkSiblings(hierarchy.getRoots(), 1.0);
        }
        for(auto const& tag : tags)
        {
            auto const index = static_cast<size_t>(std::distance(paths.cbegin(), std::find(paths.cbegin(), paths.cend(), tag.label)));
            auto const probability = std::find_if(probabilities.cbegin(), probabilities.cend(), [&](auto const& feature)
                                                  {
                                                      return feature.timestamp == tag.timestamp;
                                                  });
            check(tag.values.size() == 2 && probability != probabilities.cend() && index < probability->values.size() && tag.values.at(1) == probability->values.at(index), "the tag has the probability of its class");
        }
    }

    void testPrompt(std::string const& modelName, size_t blockSize, float hopSeconds, float windowSeconds, std::vector<std::string> const& prompts)
    {
        std::cout << "CLAP Prompt with " << modelName << ", block size " << blockSize << ", hop size " << hopSeconds << "s, window size " << windowSeconds << "s and " << prompts.size() << " prompt(s)\n";
        auto constexpr signalDuration = 6.25;
        Cvp::Prompt plugin(gSampleRate);
        if(!selectModel(plugin, modelName))
        {
            return;
        }
        plugin.setParameter("hopsize", hopSeconds);
        plugin.setParameter("windowsize", windowSeconds);
        auto const numFrames = static_cast<size_t>(signalDuration * static_cast<double>(gSampleRate));
        auto output = run(plugin, sine, numFrames, blockSize, makeMarkers(prompts));
        auto const& similarities = output[0];
        auto const& tags = output[1];
        if(prompts.empty())
        {
            check(similarities.empty() && tags.empty(), "no feature is generated without prompt");
            return;
        }
        auto const paths = plugin.getOutputDescriptors().at(0).binNames;
        auto const processedFrames = (numFrames + blockSize - 1) / blockSize * blockSize;
        auto const hopFrames = static_cast<size_t>(std::lround(static_cast<double>(hopSeconds) * static_cast<double>(gSampleRate)));
        checkWindows(similarities, processedFrames, hopFrames, paths.size());
        checkTags(tags, similarities, paths);
        checkProbabilities(Cvp::Hierarchy(prompts), output[2], similarities, tags);
    }

    void testFlatPrompts(std::string const& modelName)
    {
        std::vector<std::string> const prompts{"  a piano playing\tmusic ", "", "a dog barking", "a dog barking"};
        Cvp::Prompt plugin(gSampleRate);
        plugin.setPreComputingFeatures(makeMarkers(prompts));
        auto const paths = plugin.getOutputDescriptors().at(0).binNames;
        check(paths == std::vector<std::string>{"a piano playing music", "a dog barking"}, "the prompts are trimmed and the empty and duplicate prompts are ignored");
        testPrompt(modelName, 1024, 1.0f, 10.0f, prompts);
        testPrompt(modelName, 65536, 0.5f, 10.0f, prompts);
        testPrompt(modelName, 1024, 2.0f, 1.0f, prompts);
        testPrompt(modelName, 1024, 0.2f, 1.0f, prompts);
        testPrompt(modelName, 1024, 1.0f, 10.0f, {});
    }

    void testHierarchicalPrompts(std::string const& modelName)
    {
        std::vector<std::string> const prompts{"Music / Piano", "Animal/Dog", "Music/Guitar", "Music/Guitar/Electric guitar", "Speech"};
        Cvp::Prompt plugin(gSampleRate);
        plugin.setPreComputingFeatures(makeMarkers(prompts));
        auto const paths = plugin.getOutputDescriptors().at(0).binNames;
        std::vector<std::string> const expected{"Music", "Music/Piano", "Music/Guitar", "Music/Guitar/Electric guitar", "Animal", "Animal/Dog", "Speech"};
        check(paths == expected, "the hierarchical prompts are sorted in depth-first order");

        check(Cvp::findBest({0.3f, 0.2f, 0.25f, 0.24f, 0.1f, 0.4f, 0.2f}) == size_t(5), "the best class is the highest score among all the levels");
        check(Cvp::findBest({std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.1f}) == size_t(1), "the scores that are not finite are ignored");
        check(!Cvp::findBest({}).has_value(), "there is no best class without score");

        plugin.setParameter("hopsize", 0.23f);
        check(std::abs(plugin.getParameter("hopsize") - 0.2f) < 1.0e-6f, "the hop size is quantized to 0.1 second");
        plugin.setParameter("hopsize", 0.0f);
        check(std::abs(plugin.getParameter("hopsize") - 0.1f) < 1.0e-6f, "the hop size is at least 0.1 second");

        testPrompt(modelName, 4096, 1.0f, 5.0f, prompts);
    }

    void testExample(std::string const& modelName)
    {
        std::cout << "CLAP Example with " << modelName << "\n";
        auto constexpr signalDuration = 12.0;
        auto const numFrames = static_cast<size_t>(signalDuration * static_cast<double>(gSampleRate));
        Cvp::Example plugin(gSampleRate);
        if(!selectModel(plugin, modelName))
        {
            return;
        }
        plugin.setParameter("hopsize", 1.0f);
        plugin.setParameter("windowsize", 2.0f);

        // Two regions of tone (one without duration) and one region of noise
        Vamp::Plugin::FeatureSet input;
        auto const addRegion = [&](std::string const& label, double start, double duration)
        {
            Vamp::Plugin::Feature feature;
            feature.hasTimestamp = true;
            feature.timestamp = Vamp::RealTime::fromSeconds(start);
            feature.hasDuration = duration > 0.0;
            feature.duration = Vamp::RealTime::fromSeconds(duration);
            feature.label = label;
            input[0].push_back(feature);
        };
        addRegion("Synthetic/Tone", 1.0, 1.5);
        addRegion("Synthetic/Tone", 3.0, 0.0);
        addRegion("Synthetic/Noise", 8.0, 2.5);

        auto output = run(plugin, [&](size_t frame)
                          {
                              return toneThenNoise(frame, numFrames);
                          },
                          numFrames, 1024, input);
        auto const& similarities = output[0];
        auto const& tags = output[1];
        auto const paths = plugin.getOutputDescriptors().at(0).binNames;
        check(paths == std::vector<std::string>{"Synthetic", "Synthetic/Tone", "Synthetic/Noise"}, "the classes of the examples are sorted in depth-first order");
        auto const processedFrames = (numFrames + 1023) / 1024 * 1024;
        checkWindows(similarities, processedFrames, static_cast<size_t>(gSampleRate), paths.size());
        checkTags(tags, similarities, paths);

        // The windows that are far from the change are tagged with the right class.
        size_t numChecked = 0;
        size_t numCorrect = 0;
        for(auto const& tag : tags)
        {
            auto const center = toSeconds(tag.timestamp) + toSeconds(tag.duration) / 2.0;
            if(std::abs(center - signalDuration / 2.0) < 2.0 || center > signalDuration - 0.5)
            {
                continue;
            }
            ++numChecked;
            numCorrect += tag.label == (center < signalDuration / 2.0 ? "Synthetic/Tone" : "Synthetic/Noise") ? 1 : 0;
        }
        check(numChecked >= 6 && numCorrect == numChecked, "the windows are tagged with the class of their examples (" + std::to_string(numCorrect) + "/" + std::to_string(numChecked) + ")");

        // Without example, the plugin generates no feature.
        Cvp::Example empty(gSampleRate);
        if(selectModel(empty, modelName))
        {
            auto const emptyOutput = run(empty, sine, numFrames / 4, 1024, {});
            check(emptyOutput.empty() || std::all_of(emptyOutput.cbegin(), emptyOutput.cend(), [](auto const& pair)
                                                     {
                                                         return pair.second.empty();
                                                     }),
                  "no feature is generated without example");
        }
    }

    void testNovelty(std::string const& modelName)
    {
        std::cout << "CLAP Novelty with " << modelName << "\n";
        auto constexpr signalDuration = 12.0;
        auto const numFrames = static_cast<size_t>(signalDuration * static_cast<double>(gSampleRate));
        Cvp::Novelty plugin(gSampleRate);
        if(!selectModel(plugin, modelName))
        {
            return;
        }
        plugin.setParameter("hopsize", 0.5f);
        plugin.setParameter("windowsize", 2.0f);
        plugin.setParameter("context", 2.0f);
        auto output = run(plugin, [&](size_t frame)
                          {
                              return toneThenNoise(frame, numFrames);
                          },
                          numFrames, 1024, {});
        auto const& novelties = output[0];
        auto const& boundaries = output[1];
        auto const processedFrames = (numFrames + 1023) / 1024 * 1024;
        auto const numSegments = (processedFrames + static_cast<size_t>(gSampleRate / 2.0f) - 1) / static_cast<size_t>(gSampleRate / 2.0f);
        check(novelties.size() == numSegments, "there is one novelty value per segment");
        // The boundaries close to the beginning and the end have no window
        // entirely before or after them, their novelty is zero.
        check(!novelties.empty() && novelties.front().values.at(0) == 0.0f && novelties.back().values.at(0) == 0.0f, "the novelty is zero at the beginning and at the end");
        check(std::none_of(boundaries.cbegin(), boundaries.cend(), [](auto const& boundary)
                           {
                               return boundary.values.at(0) <= 0.0f;
                           }),
              "the boundaries have a positive novelty");
        check(std::all_of(novelties.cbegin(), novelties.cend(), [](auto const& novelty)
                          {
                              return novelty.values.at(0) >= 0.0f && novelty.values.at(0) <= 2.0f;
                          }),
              "the novelty is between 0 and 2");
        auto const maximum = std::max_element(novelties.cbegin(), novelties.cend(), [](auto const& lhs, auto const& rhs)
                                              {
                                                  return lhs.values.at(0) < rhs.values.at(0);
                                              });
        check(maximum != novelties.cend() && std::abs(toSeconds(maximum->timestamp) - signalDuration / 2.0) <= 1.0, "the maximum novelty is at the change");
        check(std::any_of(boundaries.cbegin(), boundaries.cend(), [&](auto const& boundary)
                          {
                              return std::abs(toSeconds(boundary.timestamp) - signalDuration / 2.0) <= 1.0;
                          }),
              "a boundary is detected at the change");
        for(auto const& novelty : novelties)
        {
            std::cout << "  " << toSeconds(novelty.timestamp) << "s: " << novelty.values.at(0) << "\n";
        }
        for(auto const& boundary : boundaries)
        {
            std::cout << "  boundary at " << toSeconds(boundary.timestamp) << "s (" << boundary.values.at(0) << ")\n";
        }
    }

    void testModelNames()
    {
        check(Cvp::getModelDisplayName("clap2-small-audioxl-s1B-f16.gguf") == "CLAP2 Small (F16)", "the name of a CLAP2 model is readable");
        check(Cvp::getModelDisplayName("clap2-xxs-audioxl-s1B-f32.gguf") == "CLAP2 XXS (F32)", "the size of a CLAP2 model is an acronym");
        check(Cvp::getModelDisplayName("larger_clap_general-q4_k_m.gguf") == "CLAP General (Q4_K_M)", "the name of a CLAP model is readable");
        check(Cvp::getModelDisplayName("larger_clap_music_and_speech-q8_0.gguf") == "CLAP Music and Speech (Q8_0)", "the conjunction of a CLAP model is in lower case");
        check(Cvp::getModelDisplayName("mert-v1-95m-f16.gguf") == "mert-v1-95m-f16", "the name of an unknown model is its file name");

        Cvp::Prompt plugin(gSampleRate);
        auto const parameters = plugin.getParameterDescriptors();
        auto const& names = parameters.at(0).valueNames;
        auto const paths = Cvp::getModelPaths();
        for(size_t index = 0; index < names.size() && index < paths.size(); ++index)
        {
            check(names.at(index) == std::to_string(index + 1) + ". " + Cvp::getModelDisplayName(paths.at(index)), "the models are numbered from 1 (" + names.at(index) + ")");
        }
    }
} // namespace

int main()
{
    testModelNames();
    testFlatPrompts("larger_clap_general-q4_k_m");
    testFlatPrompts("clap2-xxs-audioxl-s1B-f16");
    testHierarchicalPrompts("clap2-xxs-audioxl-s1B-f16");
    testExample("clap2-xxs-audioxl-s1B-f16");
    testExample("larger_clap_general-q4_k_m");
    testNovelty("clap2-xxs-audioxl-s1B-f16");
    testNovelty("larger_clap_general-q4_k_m");
    if(gFailures != 0)
    {
        std::cerr << gFailures << " check(s) failed\n";
        return 1;
    }
    std::cout << "All checks passed\n";
    return 0;
}
