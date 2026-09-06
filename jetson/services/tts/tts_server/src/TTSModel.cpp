#include "TTSModel.h"
#include "SynthesizerTrn.h"
#include "utils.h"

#include "vox_config.h"
#include "vox_log.h"

#include "base64.h"
#include "json.hpp"
#include "zmq.h"

TTSModel::TTSModel(const std::string &model_path)
{
    // R14：tts.engine=rk_npu 时推理下沉 RK（matcha 声学 int8 CPU + hifigan 声码器 NPU），
    // 本地 SummerTTS 不加载；REQ 失败且本地引擎可用时自动回退
    rk_mode_ = vox::config::get("tts.engine", "local") == "rk_npu";
    rk_ep_ = "tcp://" + vox::config::get("rk.ip", "192.168.137.200") + ":" +
             vox::config::get("rk.tts_port", "6720");
    last_speed_ = 1.0f;

    if (rk_mode_) {
        VOX_INFO("engine=rk_npu → %s（本地引擎%s加载作回退）", rk_ep_.c_str(),
                 model_path.empty() ? "不" : "仍");
        if (model_path.empty()) return;
    }
    load_model(model_path);
}

TTSModel::~TTSModel()
{
    if (dataW_)
    {
        tts_free_data(reinterpret_cast<int16_t *>(dataW_));
    }
}

bool TTSModel::load_model(const std::string &model_path)
{
    std::vector<char> model_path_copy(model_path.begin(), model_path.end());
    model_path_copy.push_back('\0');

    modelSize_ = ttsLoadModel(model_path_copy.data(), &dataW_);
    if (modelSize_ <= 0 || !dataW_)
    {
        return false;
    }
    synthesizer_ = std::make_unique<SynthesizerTrn>(dataW_, modelSize_);
    return true;
}

int16_t *TTSModel::infer_rk(const std::string &text, int32_t &audio_len)
{
    audio_len = 0;
    void *ctx = zmq_ctx_new();
    void *sock = zmq_socket(ctx, ZMQ_REQ);
    int timeout = 20000, linger = 0;  // 声学+声码器合成上限 20s
    zmq_setsockopt(sock, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
    zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
    if (zmq_connect(sock, rk_ep_.c_str()) != 0) {
        VOX_WARN("RK TTS connect 失败: %s", zmq_strerror(zmq_errno()));
        zmq_close(sock); zmq_ctx_destroy(ctx);
        return nullptr;
    }
    nlohmann::json req{{"text", text}};
    std::string payload = req.dump();
    if (zmq_send(sock, payload.data(), payload.size(), 0) == -1) {
        zmq_close(sock); zmq_ctx_destroy(ctx);
        return nullptr;
    }
    char buf[1 << 22];  // 4MB：~120s 音频上限
    int n = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
    zmq_close(sock);
    zmq_ctx_destroy(ctx);
    if (n <= 0) {
        VOX_WARN("RK TTS recv 失败/超时: %s", n < 0 ? zmq_strerror(zmq_errno()) : "empty");
        return nullptr;
    }
    buf[n] = '\0';
    try {
        auto rep = nlohmann::json::parse(buf, buf + n);
        if (!rep.value("ok", false)) {
            VOX_WARN("RK TTS 应答错误: %s", rep.value("err", "?").c_str());
            return nullptr;
        }
        std::string pcm_b64 = rep.at("pcm").get<std::string>();
        std::vector<uint8_t> pcm = vox::b64_decode(pcm_b64);
        int32_t frames = rep.value("frames", static_cast<int32_t>(pcm.size() / 2));
        int16_t *wav = static_cast<int16_t *>(malloc(pcm.size()));
        memcpy(wav, pcm.data(), pcm.size());
        audio_len = frames;
        last_speed_ = rep.value("sr", 22050) / 16000.0f;  // AudioPlayer 按 16000*speed 开设备
        VOX_INFO("RK TTS: %d 样本 @%dHz, %dms", frames, rep.value("sr", 22050),
                 rep.value("ms", 0));
        return wav;
    } catch (const std::exception &e) {
        VOX_WARN("RK TTS 应答解析失败: %s", e.what());
        return nullptr;
    }
}

int16_t *TTSModel::infer(const std::string &text, int32_t &audio_len)
{
    last_speed_ = 1.0f;
    if (rk_mode_) {
        int16_t *wav = infer_rk(text, audio_len);
        if (wav || !synthesizer_) return wav;
        VOX_WARN("RK TTS 失败，回退本地引擎");
    }
    if (!synthesizer_)
        return nullptr;
    return synthesizer_->infer(text, 0, 1.0, audio_len);
}

void TTSModel::free_data(int16_t *data)
{
    tts_free_data(data);
}
