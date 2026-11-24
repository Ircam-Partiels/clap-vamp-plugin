#include "cvp_model.h"
#include "cvp_models.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ggml.h>
#include <gguf.h>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <regex>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <dlfcn.h>
#else
// Prevents Windows.h from defining the min and max macros that break std::min and std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace Cvp
{
    std::string toUtf8(std::filesystem::path const& path)
    {
        auto const u8 = path.u8string();
        return std::string(u8.cbegin(), u8.cend());
    }

    // The functions of std::filesystem are used with error codes because an
    // exception must not cross the C interface of the Vamp plugin.
    static std::filesystem::path getContentsDir()
    {
        static auto const dir = []() -> std::filesystem::path
        {
            std::error_code ec;
#if !defined(_WIN32)
            Dl_info info;
            if(dladdr(reinterpret_cast<void const*>(&getContentsDir), &info) != 0 && info.dli_fname != nullptr)
            {
                auto const path = std::filesystem::absolute(std::filesystem::path(info.dli_fname), ec);
                if(!ec)
                {
                    return path.parent_path().parent_path();
                }
            }
#else
            HMODULE module = nullptr;
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPWSTR>(&getContentsDir), &module) != 0)
            {
                std::vector<wchar_t> filename(MAX_PATH);
                auto capacity = static_cast<DWORD>(filename.size());
                auto length = GetModuleFileNameW(module, filename.data(), capacity);
                while(length == capacity)
                {
                    filename.resize(filename.size() * 2);
                    capacity = static_cast<DWORD>(filename.size());
                    length = GetModuleFileNameW(module, filename.data(), capacity);
                }
                if(length != 0)
                {
                    return std::filesystem::path(filename.data()).parent_path().parent_path();
                }
            }
#endif
            return std::filesystem::current_path(ec) / "Contents";
        }();
        return dir;
    }

    static std::vector<std::filesystem::path> getModelPaths(std::filesystem::path const& directory)
    {
        std::vector<std::filesystem::path> paths;
        std::error_code ec;
        for(std::filesystem::directory_iterator it{directory, ec}; !ec && it != std::filesystem::directory_iterator{}; it.increment(ec))
        {
            if(it->path().extension() == ".gguf")
            {
                paths.push_back(it->path());
            }
        }
        // The order of a directory iteration is unspecified, the paths are sorted
        // so that the indices of the models remain identical between the sessions.
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    std::vector<std::filesystem::path> getModelPaths()
    {
        // The bundled models (generated from resource/models.txt, whose order must
        // not change) are always listed first, even if their file is missing, and
        // the models of the CLAPMODELSPATH directory are appended in alphabetical order.
        auto const modelsDir = getContentsDir() / "Resources" / "models";
        std::vector<std::filesystem::path> paths;
        for(auto const& model : gBundledModels)
        {
            paths.push_back(modelsDir / (std::string(model) + ".gguf"));
        }
#if defined(_WIN32)
        auto const* envModelPath = _wgetenv(L"CLAPMODELSPATH");
#else
        auto const* envModelPath = std::getenv("CLAPMODELSPATH");
#endif
        if(envModelPath != nullptr && *envModelPath != 0)
        {
            auto const envPaths = getModelPaths(std::filesystem::path{envModelPath});
            paths.insert(paths.end(), envPaths.cbegin(), envPaths.cend());
        }
        return paths;
    }

    std::string getModelDisplayName(std::filesystem::path const& path)
    {
        auto const stem = toUtf8(path.stem());
        auto const toUpper = [](std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c)
                           {
                               return static_cast<char>(std::toupper(c));
                           });
            return text;
        };
        auto const toWords = [](std::string const& text, char separator)
        {
            std::string result;
            auto capitalize = true;
            for(auto const c : text)
            {
                if(c == separator)
                {
                    result += ' ';
                    capitalize = true;
                }
                else
                {
                    result += capitalize ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
                    capitalize = false;
                }
            }
            return result;
        };

        // The precision of the weights is the last part of the name (f16, f32,
        // q8_0, q4_k_m...).
        static std::regex const precisionRegex("^(.+)-((f|q|bf)[0-9][0-9a-z_]*)$");
        std::smatch match;
        std::string name = stem;
        std::string precision;
        if(std::regex_match(stem, match, precisionRegex))
        {
            name = match[1].str();
            precision = toUpper(match[2].str());
        }

        // clap2-small-audioxl-s1B is displayed as CLAP2 Small (the audioxl-s1B
        // suffix is shared by all the CLAP2 models published by LAION).
        static std::regex const clap2Regex("^clap2-([a-z]+)(-audioxl-s1B)?(-.*)?$");
        // larger_clap_music_and_speech is displayed as CLAP Music and Speech
        static std::regex const clapRegex("^larger_clap_([a-z_]+)$");
        std::string displayName;
        if(std::regex_match(name, match, clap2Regex))
        {
            auto const size = match[1].str();
            // The sizes xs, xxs... are acronyms
            auto const sizeName = std::regex_match(size, std::regex("x*s")) ? toUpper(size) : toWords(size, '-');
            displayName = "CLAP2 " + sizeName + match[3].str();
        }
        else if(std::regex_match(name, match, clapRegex))
        {
            auto words = toWords(match[1].str(), '_');
            auto const andPosition = words.find(" And ");
            if(andPosition != std::string::npos)
            {
                words.replace(andPosition, 5, " and ");
            }
            displayName = "CLAP " + words;
        }
        else
        {
            return stem;
        }
        return precision.empty() ? displayName : displayName + " (" + precision + ")";
    }

    size_t getDefaultModelIndex()
    {
        auto const it = std::find_if(gBundledModels.cbegin(), gBundledModels.cend(), [](auto const& name)
                                     {
                                         return name.rfind("clap2-", 0) == 0;
                                     });
        return it != gBundledModels.cend() ? static_cast<size_t>(std::distance(gBundledModels.cbegin(), it)) : static_cast<size_t>(0);
    }

    // The models are loaded once and shared by the encoders while they are used.
    static std::shared_ptr<clap_model> acquireModel(std::string const& path)
    {
        static std::mutex mutex;
        static std::map<std::string, std::weak_ptr<clap_model>> models;
        std::lock_guard<std::mutex> lock(mutex);
        auto& entry = models[path];
        if(auto model = entry.lock())
        {
            return model;
        }
        auto const options = clap_default_options();
        clap_model* handle = nullptr;
        if(clap_model_load(path.c_str(), &options, &handle) != CLAP_OK || handle == nullptr)
        {
            std::cerr << "Failed to load the model : " << path << " (" << clap_last_error() << ")\n";
            return nullptr;
        }
        auto model = std::shared_ptr<clap_model>(handle, clap_model_free);
        entry = model;
        return model;
    }

    // Reads the logit scale of the model, that is not exposed by clap.cpp. The
    // CLAP2 models store it as "logit_scale" and the CLAP models as
    // "logit_scale_a" (the scale of the audio logits used for zero-shot
    // classification). The value is the logarithm of the temperature. The
    // models whose scale is lower than its initial value of the training
    // (log(1 / 0.07), for example larger_clap_music) or that have no scale
    // use the default temperature.
    static double readLogitScale(std::string const& path)
    {
        auto constexpr defaultScale = 100.0;
        auto const minLogScale = std::log(1.0 / 0.07);
        gguf_init_params const params{true, nullptr};
        auto* context = gguf_init_from_file(path.c_str(), params);
        if(context == nullptr)
        {
            return defaultScale;
        }
        std::optional<double> logScale;
        for(auto const* name : {"logit_scale", "logit_scale_a"})
        {
            auto const index = gguf_find_tensor(context, name);
            if(index < 0 || logScale.has_value())
            {
                continue;
            }
            auto const type = gguf_get_tensor_type(context, index);
            auto const offset = gguf_get_data_offset(context) + gguf_get_tensor_offset(context, index);
            unsigned char bytes[4]{};
            auto* file = ggml_fopen(path.c_str(), "rb");
            if(file == nullptr)
            {
                continue;
            }
#if defined(_WIN32)
            auto const seeked = _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
            auto const seeked = fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
            auto const read = seeked && std::fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
            std::fclose(file);
            if(!read)
            {
                continue;
            }
            if(type == GGML_TYPE_F32)
            {
                float value;
                std::memcpy(&value, bytes, sizeof(value));
                logScale = static_cast<double>(value);
            }
            else if(type == GGML_TYPE_F16)
            {
                ggml_fp16_t value;
                std::memcpy(&value, bytes, sizeof(value));
                logScale = static_cast<double>(ggml_fp16_to_fp32(value));
            }
        }
        gguf_free(context);
        if(!logScale.has_value() || !std::isfinite(*logScale) || *logScale < minLogScale - 1.0e-3)
        {
            return defaultScale;
        }
        return std::exp(*logScale);
    }

    std::unique_ptr<Encoder> Encoder::create(size_t modelIndex)
    {
        auto const models = getModelPaths();
        if(modelIndex >= models.size())
        {
            std::cerr << "Invalid model index : " << modelIndex << "\n";
            return nullptr;
        }
        auto const path = toUtf8(models.at(modelIndex));
        auto model = acquireModel(path);
        if(model == nullptr)
        {
            return nullptr;
        }
        auto const options = clap_default_context_options();
        CLAP* context = nullptr;
        if(clap_context_create(model.get(), &options, &context) != CLAP_OK || context == nullptr)
        {
            std::cerr << "Failed to create the context of the model : " << path << " (" << clap_last_error() << ")\n";
            return nullptr;
        }
        std::unique_ptr<Encoder> encoder(new Encoder());
        encoder->mModel = std::move(model);
        encoder->mContext = std::unique_ptr<CLAP, void (*)(CLAP*)>(context, clap_destroy);
        auto const dimension = clap_projection_dimension(context);
        if(dimension <= 0)
        {
            std::cerr << "Invalid projection dimension of the model : " << path << "\n";
            return nullptr;
        }
        encoder->mDimension = static_cast<size_t>(dimension);
        encoder->mLogitScale = readLogitScale(path);
        return encoder;
    }

    size_t Encoder::getDimension() const
    {
        return mDimension;
    }

    double Encoder::getLogitScale() const
    {
        return mLogitScale;
    }

    std::vector<float> Encoder::encodeText(std::string const& text)
    {
        std::vector<float> embedding(mDimension);
        if(clap_encode_text(mContext.get(), text.c_str(), embedding.data(), embedding.size()) != CLAP_OK)
        {
            std::cerr << "Failed to encode the text : " << text << " (" << clap_last_error() << ")\n";
            return {};
        }
        normalize(embedding);
        return embedding;
    }

    std::vector<float> Encoder::encodeAudio(float const* samples, size_t numSamples, double sampleRate)
    {
        std::vector<float> embedding(mDimension);
        // clap_encode_audio resamples the input to the sample rate of the model
        // (the CLAP models repeat an input shorter than 10 seconds).
        if(numSamples == 0 || clap_encode_audio(mContext.get(), samples, numSamples, static_cast<int>(std::lround(sampleRate)), 0, embedding.data(), embedding.size()) != CLAP_OK)
        {
            std::cerr << "Failed to encode the audio (" << (numSamples == 0 ? "no sample" : clap_last_error()) << ")\n";
            return {};
        }
        normalize(embedding);
        return embedding;
    }

    double dot(std::vector<float> const& lhs, std::vector<float> const& rhs)
    {
        auto const size = std::min(lhs.size(), rhs.size());
        auto sum = 0.0;
        for(size_t i = 0; i < size; ++i)
        {
            sum += static_cast<double>(lhs[i]) * static_cast<double>(rhs[i]);
        }
        return sum;
    }

    std::optional<size_t> findBest(std::vector<float> const& scores)
    {
        std::optional<size_t> best;
        for(size_t index = 0; index < scores.size(); ++index)
        {
            if(std::isfinite(scores[index]) && (!best.has_value() || scores[index] > scores[*best]))
            {
                best = index;
            }
        }
        return best;
    }

    std::vector<double> softmax(std::vector<float> const& scores, double scale)
    {
        std::vector<double> result(scores.size(), 0.0);
        auto const best = findBest(scores);
        if(!best.has_value())
        {
            return result;
        }
        auto const max = static_cast<double>(scores[*best]);
        auto sum = 0.0;
        for(size_t index = 0; index < scores.size(); ++index)
        {
            if(std::isfinite(scores[index]))
            {
                result[index] = std::exp(scale * (static_cast<double>(scores[index]) - max));
                sum += result[index];
            }
        }
        for(auto& value : result)
        {
            value /= sum;
        }
        return result;
    }

    void normalize(std::vector<float>& values)
    {
        auto const norm = std::sqrt(std::accumulate(values.cbegin(), values.cend(), 0.0, [](auto const sum, auto const value)
                                                    {
                                                        return sum + static_cast<double>(value) * static_cast<double>(value);
                                                    }));
        if(norm > 0.0)
        {
            std::transform(values.begin(), values.end(), values.begin(), [norm](auto const value)
                           {
                               return static_cast<float>(static_cast<double>(value) / norm);
                           });
        }
    }
} // namespace Cvp
