#pragma once

#include "cvp_hierarchy.h"
#include "cvp_model.h"
#include <IvePluginAdapter.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Cvp
{
    // The analyzer is the base class of the plugins. It loads the model and
    // generates the embeddings of the audio windows. The audio is divided into
    // consecutive segments of Hop Size duration and each segment is analyzed
    // with a window of Window Size duration centered on the segment (the window
    // is shortened at the beginning and at the end of the audio stream).
    //
    // Example with a hop size of 1 second and a window size of 3 seconds (the
    // dots are the parts of the windows outside the audio stream):
    //
    // Time (s)          0     1     2     3     4     5     6
    //                   |-----|-----|-----|-----|-----|-----|
    // Segments          [ S0 ][ S1 ][ S2 ][ S3 ][ S4 ][ S5 ]
    // Window S0   ......[====W0====]
    // Window S1         [=======W1=======]
    // Window S2               [=======W2=======]
    // Window S3                     [=======W3=======]
    // Window S4                           [=======W4=======]
    // Window S5                                 [====W5====]......
    //
    // The segment k covers [k * hop, (k + 1) * hop[ and its features are
    // positioned on it (the last segment is shorter if the duration of the
    // audio stream is not a multiple of the hop size). The window k covers
    // [(k + 0.5) * hop - window / 2, (k + 0.5) * hop + window / 2[ and the
    // segment is analyzed once the end of its window is received.
    class Analyzer
    : public Vamp::Plugin
    , public Ive::PluginExtension
    {
    public:
        Analyzer(float inputSampleRate, float defaultWindowSeconds);
        ~Analyzer() override = default;

        // Vamp::Plugin
        bool initialise(size_t channels, size_t stepSize, size_t blockSize) override;

        InputDomain getInputDomain() const override;
        std::string getMaker() const override;
        int getPluginVersion() const override;
        std::string getCopyright() const override;

        size_t getPreferredBlockSize() const override;
        size_t getPreferredStepSize() const override;

        void reset() override;
        FeatureSet process(float const* const* inputBuffers, Vamp::RealTime timestamp) override;
        FeatureSet getRemainingFeatures() override;

        ParameterList getParameterDescriptors() const override;
        float getParameter(std::string paramid) const override;
        void setParameter(std::string paramid, float newval) override;

    protected:
        // Prepares the analysis before processing the first block (for example,
        // encodes the text prompts). The analyzer calls the method once after
        // each initialization or reset, when the first block is processed (and
        // not in initialise(), because the hosts set the precomputed features
        // after the initialization). If the method returns false, the audio is
        // ignored and no feature is generated until the next reset.
        virtual bool prepareBeforeFirstBlock();

        // Processes the embedding of an audio segment once its window is
        // analyzed. The embedding is empty if the encoding failed.
        virtual void processSegment(size_t segment, Vamp::RealTime const& timestamp, Vamp::RealTime const& duration, std::vector<float> const& embedding, FeatureSet& fs) = 0;

        // Called once all the segments are analyzed (in getRemainingFeatures)
        virtual void finish(FeatureSet& fs);

        // Called by reset() to clear the analysis state
        virtual void clear();

        // Returns the number of samples of the audio stream received since the last reset
        size_t getReceivedSamples() const;

        Encoder* getEncoder() const;
        size_t getBlockSize() const;
        size_t getWindowSize() const;
        size_t getHopSize() const;

        static Vamp::RealTime toRealTime(size_t samples, double sampleRate);

    private:
        // Calls prepareBeforeFirstBlock() if it has not been called since the
        // last reset and returns its result.
        bool isPrepared();
        void analyse(size_t endPosition, FeatureSet& fs);

        float const mDefaultWindowSeconds;
        std::unique_ptr<Encoder> mEncoder;
        size_t mEncoderModelIndex{0};

        size_t mModelIndex;
        double mWindowSeconds;
        double mHopSeconds{1.0};
        size_t mBlockSize{0};

        std::optional<bool> mPrepared;
        std::vector<float> mBuffer;
        size_t mBufferPosition{0};
        size_t mReceivedSamples{0};
        size_t mNextSegment{0};
    };

    // Zero-shot audio tagging with text prompts
    class Prompt
    : public Analyzer
    {
    public:
        Prompt(float inputSampleRate);
        ~Prompt() override = default;

        std::string getIdentifier() const override;
        std::string getName() const override;
        std::string getDescription() const override;

        OutputList getOutputDescriptors() const override;

        // Ive::PluginExtension
        InputList getInputDescriptors() const override;
        void setPreComputingFeatures(FeatureSet const& fs) override;
        OutputExtraList getOutputExtraDescriptors(size_t outputDescriptorIndex) const override;

    private:
        bool prepareBeforeFirstBlock() override;
        void processSegment(size_t segment, Vamp::RealTime const& timestamp, Vamp::RealTime const& duration, std::vector<float> const& embedding, FeatureSet& fs) override;

        Hierarchy mHierarchy;
        std::vector<std::vector<float>> mTextEmbeddings;
    };

    // Audio tagging and query by example with annotated audio regions
    class Example
    : public Analyzer
    {
    public:
        Example(float inputSampleRate);
        ~Example() override = default;

        std::string getIdentifier() const override;
        std::string getName() const override;
        std::string getDescription() const override;

        OutputList getOutputDescriptors() const override;

        // Ive::PluginExtension
        InputList getInputDescriptors() const override;
        void setPreComputingFeatures(FeatureSet const& fs) override;
        OutputExtraList getOutputExtraDescriptors(size_t outputDescriptorIndex) const override;

        // Captures the audio of the regions and analyzes the block
        FeatureSet process(float const* const* inputBuffers, Vamp::RealTime timestamp) override;

    private:
        bool prepareBeforeFirstBlock() override;
        void processSegment(size_t segment, Vamp::RealTime const& timestamp, Vamp::RealTime const& duration, std::vector<float> const& embedding, FeatureSet& fs) override;
        void finish(FeatureSet& fs) override;
        void clear() override;

        struct Region
        {
            std::string path;
            double start;
            double duration;
        };

        // The audio of a region is captured while the audio stream is received,
        // and encoded by chunks of window size whose embeddings are averaged.
        struct Capture
        {
            std::string path;
            size_t start;
            size_t end;
            std::vector<float> chunk;
            std::vector<double> sum;
            size_t numChunks{0};
        };

        struct Segment
        {
            Vamp::RealTime timestamp;
            Vamp::RealTime duration;
            std::vector<float> embedding;
        };

        void captureRegions(float const* samples, size_t numSamples);
        void encode(Capture& capture);

        Hierarchy mHierarchy;
        std::vector<Region> mRegions;
        std::vector<Capture> mCaptures;
        size_t mCapturePosition{0};
        std::vector<Segment> mSegments;
    };

    // Detection of the changes in the audio stream (novelty curve and boundaries)
    class Novelty
    : public Analyzer
    {
    public:
        Novelty(float inputSampleRate);
        ~Novelty() override = default;

        std::string getIdentifier() const override;
        std::string getName() const override;
        std::string getDescription() const override;

        OutputList getOutputDescriptors() const override;

        ParameterList getParameterDescriptors() const override;
        float getParameter(std::string paramid) const override;
        void setParameter(std::string paramid, float newval) override;

        // Ive::PluginExtension
        void setPreComputingFeatures(FeatureSet const& fs) override;
        OutputExtraList getOutputExtraDescriptors(size_t outputDescriptorIndex) const override;

    private:
        void processSegment(size_t segment, Vamp::RealTime const& timestamp, Vamp::RealTime const& duration, std::vector<float> const& embedding, FeatureSet& fs) override;
        void finish(FeatureSet& fs) override;
        void clear() override;

        std::vector<std::vector<float>> mEmbeddings;
        std::vector<Vamp::RealTime> mTimestamps;
        double mContextSeconds{5.0};
    };
} // namespace Cvp
