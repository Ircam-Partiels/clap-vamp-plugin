#pragma once

#include <clap.h>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Cvp
{
    // Clap.cpp expects UTF-8 paths while std::filesystem::path::string() uses
    // the ANSI code page on Windows (and throws for unrepresentable characters).
    std::string toUtf8(std::filesystem::path const& path);

    // Returns the paths of the models: the bundled models (in the order of
    // resource/models.txt) followed by the models of the CLAPMODELSPATH
    // directory in alphabetical order. The index of a model is saved in the
    // documents of the hosts, hence the order must remain stable.
    std::vector<std::filesystem::path> getModelPaths();

    // Returns the readable name of a model from its file name, for example
    // "CLAP2 Small (F16)" for clap2-small-audioxl-s1B-f16.gguf or "CLAP Music and
    // Speech (Q8_0)" for larger_clap_music_and_speech-q8_0.gguf (the file name
    // without extension is used for the other models).
    std::string getModelDisplayName(std::filesystem::path const& path);

    // Returns the index of the default model (the first CLAP2 bundled model, or
    // the first model if there is none).
    size_t getDefaultModelIndex();

    // The encoder generates the normalized embeddings of the texts and the audio
    // with a model. The weights of a model are shared by all the encoders that
    // use it (for example, the instances of the plugins of several tracks), but
    // each encoder has its own execution context, hence the encoders can be
    // used concurrently.
    class Encoder
    {
    public:
        // Returns nullptr if the model cannot be loaded (the error is logged).
        static std::unique_ptr<Encoder> create(std::filesystem::path const& modelPath);

        size_t getDimension() const;

        // Returns the temperature of the model that scales the similarities
        // between the audio and the texts before the softmax to obtain
        // probabilities (the exponential of the logit scale learned by the
        // model, or 100 if the model has no trained logit scale).
        double getLogitScale() const;

        // Returns the normalized embedding (hence the dot product of two
        // embeddings is their cosine similarity), or an empty embedding if the
        // encoding failed (the error is logged).
        std::vector<float> encodeText(std::string const& text);
        std::vector<float> encodeAudio(float const* samples, size_t numSamples, double sampleRate);

    private:
        Encoder() = default;

        std::shared_ptr<clap_model> mModel;
        std::unique_ptr<CLAP, void (*)(CLAP*)> mContext{nullptr, nullptr};
        size_t mDimension{0};
        double mLogitScale{100.0};
    };

    // Utilities on the embeddings
    double dot(std::vector<float> const& lhs, std::vector<float> const& rhs);
    void normalize(std::vector<float>& values);

    // Returns the index of the highest score (the scores that are not finite
    // are ignored), or nothing if there is no finite score
    std::optional<size_t> findBest(std::vector<float> const& scores);

    // Returns the softmax of the scores multiplied by the scale
    std::vector<double> softmax(std::vector<float> const& scores, double scale);
} // namespace Cvp
