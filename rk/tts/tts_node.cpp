// tts_node.cpp — RK 端 TTS 推理节点（R14：TTS 推理下沉 RK）
//
// 引擎 = SummerTTS（与 Jetson 本地引擎同一套 vendored 源码，RK3588 CPU/Eigen 推理）；
// 协议 = Jetson tts_server rk_npu 分支的 REP 协议：
//   请求 {"text": "..."} → 应答 {"ok":true,"sr":16000,"frames":N,"ms":T,"pcm":"<b64 int16>"}
// Jetson 侧 AudioPlayer 按 sr/16000 定播放速率，本引擎输出 16k → speed=1.0。
//
// 立场说明（面试口径）：ASR 走 RKNN fp16 NPU；TTS 曾尝试 matcha-icefall RKNN 量化
// 部署，声码器（vocos）输出为 STFT 谱、需主机侧 ISTFT，NPU 图不适用，最终按
// 参考工程（LLM_Voice_Flow）口径改用 SummerTTS CPU 引擎上板——嵌入式 TTS 落地
// 以稳定可听为先，量化收益（RTF 已 <0.2）不构成冒险理由。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>

#include <zmq.h>

#include "SynthesizerTrn.h"
#include "utils.h"

#include "base64.h"
#include "json.hpp"

namespace {

void log(const char *tag, const char *fmt, ...) {
    char timebuf[16];
    std::time_t t = std::time(nullptr);
    std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", std::localtime(&t));
    std::printf("[%s] [%s] ", timebuf, tag);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

// 与 voice_service 同款扁平 key=value 解析（voxdrive.rk.conf）
std::map<std::string, std::string> load_conf(const std::string &path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k, v;
        for (char c : line.substr(0, eq))
            if (c != ' ' && c != '\t' && c != '\r') k += c;
        size_t s = line.find_first_not_of(" \t", eq + 1);
        if (s == std::string::npos) continue;
        v = line.substr(s);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
        kv[k] = v;
    }
    return kv;
}

std::string conf_get(const std::map<std::string, std::string> &kv, const std::string &k,
                     const std::string &dft) {
    auto it = kv.find(k);
    return it == kv.end() ? dft : it->second;
}

std::string expand_home(const std::string &p) {
    if (p.rfind("$HOME", 0) == 0) {
        const char *h = std::getenv("HOME");
        return std::string(h ? h : "") + p.substr(5);
    }
    return p;
}

}  // namespace

int main(int argc, char **argv) {
    std::string conf_path = argc > 1 ? argv[1] : "jetson/config/voxdrive.rk.conf";
    auto kv = load_conf(conf_path);
    std::string model_path =
        expand_home(conf_get(kv, "voice.tts_bin", "$HOME/rk_tts/single_speaker_fast.bin"));
    std::string port = conf_get(kv, "voice.tts_rep", "6720");
    float length_scale = std::atof(conf_get(kv, "voice.tts_length_scale", "1.0").c_str());

    std::vector<char> path(model_path.begin(), model_path.end());
    path.push_back('\0');
    float *model_data = nullptr;
    int32_t model_size = ttsLoadModel(path.data(), &model_data);
    if (model_size <= 0 || !model_data) {
        log("tts", "模型加载失败: %s", model_path.c_str());
        return 1;
    }
    SynthesizerTrn synth(model_data, model_size);
    log("tts", "SummerTTS 引擎就绪 (%.1fMB): %s", model_size / 1048576.0, model_path.c_str());

    void *ctx = zmq_ctx_new();
    void *rep = zmq_socket(ctx, ZMQ_REP);
    int linger = 0;
    zmq_setsockopt(rep, ZMQ_LINGER, &linger, sizeof(linger));
    std::string ep = "tcp://*:" + port;
    if (zmq_bind(rep, ep.c_str()) != 0) {
        log("tts", "bind %s 失败: %s", ep.c_str(), zmq_strerror(zmq_errno()));
        return 1;
    }
    log("tts", "REP :%s 就绪，等 Jetson 合成请求", port.c_str());

    char buf[256];
    while (true) {
        int n = zmq_recv(rep, buf, sizeof(buf) - 1, 0);
        if (n < 0) continue;
        buf[n] = '\0';
        std::string text;
        try {
            auto req = nlohmann::json::parse(buf, buf + n);
            text = req.value("text", "");
        } catch (const std::exception &) {
            zmq_send(rep, "{\"ok\":false,\"err\":\"bad json\"}", 28, 0);
            continue;
        }
        if (text.empty()) {
            zmq_send(rep, "{\"ok\":false,\"err\":\"empty text\"}", 30, 0);
            continue;
        }
        std::clock_t c0 = std::clock();
        int32_t audio_len = 0;
        int16_t *wav = synth.infer(text, 0, length_scale, audio_len);
        int ms = static_cast<int>((std::clock() - c0) * 1000 / CLOCKS_PER_SEC);
        if (!wav || audio_len <= 0) {
            log("tts", "synth 失败: %s", text.c_str());
            zmq_send(rep, "{\"ok\":false,\"err\":\"synth failed\"}", 31, 0);
            continue;
        }
        std::string pcm_b64 =
            vox::b64_encode(reinterpret_cast<const uint8_t *>(wav),
                            static_cast<size_t>(audio_len) * sizeof(int16_t));
        tts_free_data(wav);
        nlohmann::json rep_json{{"ok", true}, {"sr", 16000},
                                 {"frames", audio_len}, {"ms", ms}, {"pcm", pcm_b64}};
        std::string payload = rep_json.dump();
        zmq_send(rep, payload.data(), payload.size(), 0);
        log("tts", "synth %s (%d 样本 %dms)", text.substr(0, 24).c_str(), audio_len, ms);
    }
}
