#ifndef TTS_MODEL_H
#define TTS_MODEL_H

#include <memory>
#include <string>
#include <stdio.h>
#include "SynthesizerTrn.h"
#include "utils.h"
#include "Hanz2Piny.h"
#include "hanzi2phoneid.h"
#include <iostream>
#include <fstream>

// 前向声明
class SynthesizerTrn;

class TTSModel {
public:
    explicit TTSModel(const std::string& model_path);
    ~TTSModel();
    
    bool load_model(const std::string& model_path);
    int16_t* infer(const std::string& text, int32_t& audio_len);
    void free_data(int16_t* data);

    // R14：RK NPU 推理模式下最近一次合成音频的播放速率（AudioPlayer 按 16000*speed 开设备）
    float last_speed() const { return last_speed_; }

private:
    int16_t* infer_rk(const std::string& text, int32_t& audio_len);

    float* dataW_ = nullptr;
    int32_t modelSize_ = 0;
    std::unique_ptr<SynthesizerTrn> synthesizer_;
    bool rk_mode_ = false;
    std::string rk_ep_;
    float last_speed_ = 1.0f;
};

#endif // TTS_MODEL_H