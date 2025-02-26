#pragma once
#include <map>
#include "vector"
#include <string>
#include "fstream"
#include "thread"

#include "opencv2/opencv.hpp"
#include "onnxruntime_cxx_api.h"

#include "sample_log.h"
#include "Tokenizer.hpp"

// struct CLIP_IMAG_FEATURE_T
// {
//     float feature[LEN_IMAGE_FEATURE];
// };

// struct CLIP_TEXT_FEATURE_T
// {
//     int feature[LEN_TEXT_FEATURE];
// };

class CLIP
{
protected:
    std::string device{"cpu"};
    Ort::Env env;
    Ort::SessionOptions session_options;
    std::shared_ptr<Ort::Session> TextEncoderSession, DecoderSession;
    Ort::MemoryInfo memory_info_handler = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    const char
        *TextEncInputNames[1]{"texts"},
        *TextEncOutputNames[1]{"text_features"},
        *DecoderInputNames[2]{"image_features", "text_features"},
        *DecoderOutputNames[2]{"logits_per_image", "logits_per_text"};
    float _mean_val[3] = {0.48145466f * 255.f, 0.4578275f * 255.f, 0.40821073f * 255.f};
    float _std_val[3] = {1 / (0.26862954f * 255.f), 1 / (0.26130258f * 255.f), 1 / (0.27577711f * 255.f)};
    std::shared_ptr<TokenizerBase> tokenizer;

    std::vector<float> image_features_input;
    std::vector<float> text_features_input;
    std::vector<int> text_tokens_input;

    bool _isCN = false;
    int LEN_IMAGE_FEATURE = 512;
    int LEN_TEXT_FEATURE = 512;
    int LEN_TEXT_TOKEN = 77;

public:
    CLIP()
    {
        env = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "CLIP_DECODER");
        session_options = Ort::SessionOptions();
        session_options.SetInterOpNumThreads(std::thread::hardware_concurrency());
        session_options.SetIntraOpNumThreads(std::thread::hardware_concurrency());
        // 设置图像优化级别
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    }

    bool load_tokenizer(std::string vocab_path, bool isCN)
    {
        _isCN = isCN;
        if (isCN)
        {
            LEN_TEXT_TOKEN = 52;
            tokenizer.reset(new TokenizerClipChinese);
        }
        else
        {
            tokenizer.reset(new TokenizerClip);
        }
        ALOGI("text token len %d", LEN_TEXT_TOKEN);
        text_tokens_input = std::vector<int>(1024 * LEN_TEXT_TOKEN);
        return tokenizer->load_tokenize(vocab_path);
    }

    bool load_decoder(std::string decoder_path)
    {
        DecoderSession.reset(new Ort::Session(env, decoder_path.c_str(), session_options));
        if (DecoderSession->GetInputCount() != 2 || DecoderSession->GetOutputCount() != 2)
        {
            ALOGE("Model not loaded (invalid input/output count)");
            return false;
        }
        return true;
    }

    bool load_text_encoder(std::string encoder_path)
    {
        TextEncoderSession.reset(new Ort::Session(env, encoder_path.c_str(), session_options));
        if (TextEncoderSession->GetInputCount() != 1 || TextEncoderSession->GetOutputCount() != 1)
        {
            ALOGE("Model not loaded (invalid input/output count)");
            return false;
        }
        auto shape = TextEncoderSession->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        LEN_TEXT_FEATURE = shape[1];
        ALOGI("text feature len %d", LEN_TEXT_FEATURE);
        text_features_input = std::vector<float>(1024 * LEN_TEXT_FEATURE);
        return true;
    }

    virtual bool load_image_encoder(std::string encoder_path) = 0;
    virtual void encode(cv::Mat image, std::vector<float> &image_features) = 0;

    void encode(std::vector<std::string> &texts, std::vector<std::vector<float>> &text_features)
    {
        std::vector<std::vector<int>> text_token;
        text_token.resize(texts.size());
        for (size_t i = 0; i < texts.size(); i++)
        {
            tokenizer->encode_text(texts[i], text_token[i]);
        }

        if (text_token.size() * LEN_TEXT_TOKEN > text_tokens_input.size())
        {
            text_tokens_input.resize(text_token.size() * LEN_TEXT_TOKEN);
        }

        memset(text_tokens_input.data(), 0, text_token.size() * LEN_TEXT_TOKEN * sizeof(int));
        auto text_tokens_input_ptr = text_tokens_input.data();
        for (size_t i = 0; i < text_token.size(); i++)
        {
            if (text_token[i].size() > LEN_TEXT_TOKEN)
            {
                ALOGW("text_features index %d ,bigger than %d\n", i, LEN_TEXT_TOKEN);
                continue;
            }
            memcpy(text_tokens_input_ptr + i * LEN_TEXT_TOKEN, text_token[i].data(), text_token[i].size() * sizeof(int));
        }

        if (_isCN)
        {
            std::vector<int64_t> text_token_shape = {1, LEN_TEXT_TOKEN};
            text_features.resize(text_token.size());

            std::vector<int64> text_tokens_input_64(texts.size() * LEN_TEXT_TOKEN);
            for (size_t i = 0; i < text_tokens_input_64.size(); i++)
            {
                text_tokens_input_64[i] = text_tokens_input[i];
            }

            for (size_t i = 0; i < text_token.size(); i++)
            {
                auto inputTensor = (Ort::Value::CreateTensor<int64>(
                    memory_info_handler, text_tokens_input_64.data() + i * LEN_TEXT_TOKEN, LEN_TEXT_TOKEN, text_token_shape.data(), text_token_shape.size()));

                Ort::RunOptions runOptions;
                auto OutputTensors = TextEncoderSession->Run(runOptions, TextEncInputNames, &inputTensor,
                                                             1, TextEncOutputNames, 1);
                auto &text_features_tensor = OutputTensors[0];
                auto text_features_tensor_ptr = text_features_tensor.GetTensorMutableData<float>();

                text_features[i].resize(LEN_TEXT_FEATURE);
                memcpy(text_features[i].data(), text_features_tensor_ptr, LEN_TEXT_FEATURE * sizeof(float));
            }
        }
        else
        {
            std::vector<int64_t> text_token_shape = {(int64_t)text_token.size(), LEN_TEXT_TOKEN};

            auto inputTensor = (Ort::Value::CreateTensor<int>(
                memory_info_handler, text_tokens_input.data(), text_tokens_input.size(), text_token_shape.data(), text_token_shape.size()));

            Ort::RunOptions runOptions;
            auto OutputTensors = TextEncoderSession->Run(runOptions, TextEncInputNames, &inputTensor,
                                                         1, TextEncOutputNames, 1);
            auto &text_features_tensor = OutputTensors[0];
            auto text_features_tensor_ptr = text_features_tensor.GetTensorMutableData<float>();
            auto output_shape = text_features_tensor.GetTensorTypeAndShapeInfo().GetShape();

            text_features.resize(output_shape[0]);

            for (size_t i = 0; i < text_features.size(); i++)
            {
                text_features[i].resize(output_shape[1]);
                memcpy(text_features[i].data(), text_features_tensor_ptr + i * output_shape[1], output_shape[1] * sizeof(float));
            }
        }
    }

    void decode(std::vector<std::vector<float>> &image_features, std::vector<std::vector<float>> &text_features,
                std::vector<std::vector<float>> &logits_per_image, std::vector<std::vector<float>> &logits_per_text)
    {
        if (image_features.size() * LEN_IMAGE_FEATURE > image_features_input.size())
        {
            image_features_input.resize(image_features.size() * LEN_IMAGE_FEATURE);
        }
        if (text_features.size() * LEN_TEXT_FEATURE > text_features_input.size())
        {
            text_features_input.resize(text_features.size() * LEN_TEXT_FEATURE);
        }

        memset(image_features_input.data(), 0, image_features_input.size() * sizeof(float));
        auto image_features_input_ptr = image_features_input.data();
        for (size_t i = 0; i < image_features.size(); i++)
        {
            if (image_features[i].size() != LEN_IMAGE_FEATURE)
            {
                ALOGW("image_features index %d ,not equal %d\n", i, LEN_IMAGE_FEATURE);
                continue;
            }
            memcpy(image_features_input_ptr + i * LEN_IMAGE_FEATURE, image_features[i].data(), LEN_IMAGE_FEATURE * sizeof(float));
        }

        memset(text_features_input.data(), 0, text_features_input.size() * sizeof(float));
        auto text_features_input_ptr = text_features_input.data();
        for (size_t i = 0; i < text_features.size(); i++)
        {
            if (text_features[i].size() != LEN_TEXT_FEATURE)
            {
                ALOGW("text_features index %d ,not equal %d\n", i, LEN_TEXT_FEATURE);
                continue;
            }
            memcpy(text_features_input_ptr + i * LEN_TEXT_FEATURE, text_features[i].data(), text_features[i].size() * sizeof(float));
        }
        std::vector<Ort::Value> inputTensors;

        std::vector<int64_t> image_features_shape = {(int64_t)image_features.size(), LEN_IMAGE_FEATURE};
        std::vector<int64_t> text_features_shape = {(int64_t)text_features.size(), LEN_TEXT_FEATURE};

        inputTensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, image_features_input.data(), image_features_input.size(), image_features_shape.data(), image_features_shape.size()));
        inputTensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, text_features_input.data(), text_features_input.size(), text_features_shape.data(), text_features_shape.size()));

        Ort::RunOptions runOptions;
        auto DecoderOutputTensors = DecoderSession->Run(runOptions, DecoderInputNames, inputTensors.data(),
                                                        inputTensors.size(), DecoderOutputNames, 2);

        auto &logits_per_image_output = DecoderOutputTensors[0];
        auto logits_per_image_ptr = logits_per_image_output.GetTensorMutableData<float>();
        auto logits_per_image_shape = logits_per_image_output.GetTensorTypeAndShapeInfo().GetShape();
        logits_per_image.resize(logits_per_image_shape[0]);
        for (size_t i = 0; i < logits_per_image.size(); i++)
        {
            logits_per_image[i].resize(logits_per_image_shape[1]);
            memcpy(logits_per_image[i].data(), logits_per_image_ptr + i * logits_per_image_shape[1], logits_per_image_shape[1] * sizeof(float));
        }

        auto &logits_per_text_output = DecoderOutputTensors[1];
        auto logits_per_text_ptr = logits_per_text_output.GetTensorMutableData<float>();
        auto logits_per_text_shape = logits_per_text_output.GetTensorTypeAndShapeInfo().GetShape();
        logits_per_text.resize(logits_per_text_shape[0]);
        for (size_t i = 0; i < logits_per_text.size(); i++)
        {
            logits_per_text[i].resize(logits_per_text_shape[1]);
            memcpy(logits_per_text[i].data(), logits_per_text_ptr + i * logits_per_text_shape[1], logits_per_text_shape[1] * sizeof(float));
        }
    }

    void decode(std::vector<float> &image_features, std::vector<int> &text_features,
                std::vector<std::vector<float>> &logits_per_image, std::vector<std::vector<float>> &logits_per_text)
    {
        std::vector<Ort::Value> inputTensors;

        std::vector<int64_t> image_features_shape = {(int64_t)image_features.size() / LEN_IMAGE_FEATURE, LEN_IMAGE_FEATURE};
        std::vector<int64_t> text_features_shape = {(int64_t)text_features.size() / LEN_TEXT_FEATURE, LEN_TEXT_FEATURE};

        inputTensors.push_back(Ort::Value::CreateTensor<float>(
            memory_info_handler, image_features.data(), image_features_shape[0] * image_features_shape[1], image_features_shape.data(), image_features_shape.size()));
        inputTensors.push_back(Ort::Value::CreateTensor<int>(
            memory_info_handler, text_features.data(), text_features_shape[0] * text_features_shape[1], text_features_shape.data(), text_features_shape.size()));

        Ort::RunOptions runOptions;
        auto DecoderOutputTensors = DecoderSession->Run(runOptions, DecoderInputNames, inputTensors.data(),
                                                        inputTensors.size(), DecoderOutputNames, 2);

        auto &logits_per_image_output = DecoderOutputTensors[0];
        auto logits_per_image_ptr = logits_per_image_output.GetTensorMutableData<float>();
        auto logits_per_image_shape = logits_per_image_output.GetTensorTypeAndShapeInfo().GetShape();
        logits_per_image.resize(logits_per_image_shape[0]);
        for (size_t i = 0; i < logits_per_image.size(); i++)
        {
            logits_per_image[i].resize(logits_per_image_shape[1]);
            memcpy(logits_per_image[i].data(), logits_per_image_ptr + i * logits_per_image_shape[1], logits_per_image_shape[1] * sizeof(float));
        }

        auto &logits_per_text_output = DecoderOutputTensors[1];
        auto logits_per_text_ptr = logits_per_text_output.GetTensorMutableData<float>();
        auto logits_per_text_shape = logits_per_text_output.GetTensorTypeAndShapeInfo().GetShape();
        logits_per_text.resize(logits_per_text_shape[0]);
        for (size_t i = 0; i < logits_per_text.size(); i++)
        {
            logits_per_text[i].resize(logits_per_text_shape[1]);
            memcpy(logits_per_text[i].data(), logits_per_text_ptr + i * logits_per_text_shape[1], logits_per_text_shape[1] * sizeof(float));
        }
    }
};
