// tts_server — TTS 服务（VITS/SummerTTS 推理 + ALSA 播放）
//
// 三端口半双工握手协议（与 prj1 的 ASR/intent_router/keyboard_test 兼容）：
//   :port.tts_block REQ "block" → "ok"            阻塞门：ASR 侧置位，防播报期回灌
//   :port.tts_text  REQ "<文本> END" → "Echo: received"  播报文本（" END" 为单次播报结束标记）
//   :port.tts_pub   PUB "play_end"                整次播报完成事件（dashboard/测试端等待）
// 模型路径：argv[1] 覆盖，缺省读 conf model.tts；声卡设备 conf tts.alsa_device。
#define VOX_LOG_TAG "tts"
#include "vox_config.h"
#include "vox_log.h"

#include "AudioPlayer.h"
#include "MessageQueue.h"
#include "TTSModel.h"
#include "TextProcessor.h"
#include "Utils.h"
#include "ZmqPub.h"
#include "ZmqServer.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

// 半双工握手状态：true = 当前音频段为一次播报的末段（播放后发布 play_end）
static std::atomic<bool> first_msg{true};

void synthesis_worker(DoubleMessageQueue &queue, TTSModel &model) {
    while (true) {
        std::string text = queue.pop_text();
        if (text.empty()) break;  // queue.stop() 停止信号
        VOX_DEBUG("synth popped %zu bytes", text.size());

        if (text.find("END") != std::string::npos) {
            first_msg = true;
            text = text.substr(0, text.find("END"));
        }

        int32_t audio_len = 0;
        if (!text.empty()) {
            VOX_INFO("synth infer: %.60s", text.c_str());
            int16_t *wav = model.infer(text, audio_len);
            if (wav && audio_len > 0) {
                auto audio = std::make_unique<int16_t[]>(audio_len);
                memcpy(audio.get(), wav, audio_len * sizeof(int16_t));
                queue.push_audio(std::move(audio), audio_len, first_msg);
                model.free_data(wav);
            }
        } else {
            queue.push_audio(std::make_unique<int16_t[]>(0), 0, first_msg);
        }
    }
}

void playback_worker(DoubleMessageQueue &queue, AudioPlayer &player,
                     zmq_component::ZmqPub &play_end_pub) {
    while (true) {
        auto msg = queue.pop_audio();
        if (msg.data == nullptr) break;
        VOX_DEBUG("play %d samples, is_last=%d", msg.length, msg.is_last ? 1 : 0);
        player.play(msg.data.get(), msg.length * sizeof(int16_t), 1.0f);
        if (msg.is_last) {
            VOX_INFO("play_end 发布");
            play_end_pub.publish("play_end");
        }
    }
}

int main(int argc, char **argv) {
    if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，端口/模型路径使用内置默认值");

    const std::string model_path = argc > 1 ? argv[1] : vox::config::get("model.tts", "");
    if (model_path.empty()) {
        VOX_ERROR("缺少模型路径（argv[1] 或 conf model.tts）");
        return 1;
    }

    try {
        // conf 加载后再构造三端口（6677 阻塞门 / 7777 文本 / 6678 play_end）
        zmq_component::ZmqServer status_server(
            vox::config::bind_endpoint("port.tts_block", "6677"));
        zmq_component::ZmqServer server(
            vox::config::bind_endpoint("port.tts_text", "7777"));
        zmq_component::ZmqPub play_end_pub(
            vox::config::bind_endpoint("port.tts_pub", "6678"));

        TTSModel model(model_path);
        AudioPlayer player;
        DoubleMessageQueue queue;

        std::thread synthesis_thread(synthesis_worker, std::ref(queue), std::ref(model));
        std::thread playback_thread(playback_worker, std::ref(queue), std::ref(player),
                                    std::ref(play_end_pub));

        VOX_INFO("model=%s, block %s, text %s, pub %s", model_path.c_str(),
                 vox::config::bind_endpoint("port.tts_block", "").c_str(),
                 vox::config::bind_endpoint("port.tts_text", "").c_str(),
                 vox::config::bind_endpoint("port.tts_pub", "").c_str());

        while (true) {
            // 先收阻塞门请求并立即应答，再收文本——保证 ASR 侧握手的顺序语义
            std::string req = status_server.receive();
            VOX_INFO("[->block] %.30s", req.c_str());
            status_server.send("ok");

            first_msg = false;
            std::string text = server.receive();
            server.send("Echo: received");
            VOX_INFO("[->text] %.120s", text.c_str());

            if (!text.empty() && text.find("<think>") == std::string::npos) {
                queue.push_text(text);
            }
        }
        // 主循环常驻不退出；worker 的 queue.stop() 清理路径仅进程终止时由 OS 回收
    } catch (const std::exception &e) {
        VOX_ERROR("启动失败: %s", e.what());
        return 1;
    }

    return 0;
}
