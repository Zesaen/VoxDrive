// mic_stream — 麦克风 PCM 流转发（R14：ASR 推理下沉 RK NPU 后的 Jetson 侧采集件）
//
// PortAudio 采集 16kHz float32 mono → ZMQ PUSH 发往 RK voice_service :rk.asr_port。
// 推理/终点/意图路由全部在 RK 侧完成（voice_service.py REQ intent_router + tts_block），
// 本进程只负责采音转发，Ctrl+C 退出。
//
// 目标地址：conf rk.ip + rk.asr_port（--push 可覆盖，如 tcp://192.168.137.200:6711）。
// 麦克风：环境变量 SHERPA_ONNX_MIC_DEVICE > conf asr.mic_device > PortAudio 默认。
#define VOX_LOG_TAG "asr"
#include "vox_config.h"
#include "vox_log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <string>
#include <vector>

#include "ZmqClient.h"
#include "portaudio.h"  // NOLINT
#include "zmq.h"

static std::atomic<bool> stop_{false};

static void Handler(int /*sig*/) {
  stop_ = true;
  fprintf(stderr, "\nCaught Ctrl + C. Exiting...\n");
}

int32_t main(int32_t argc, char *argv[]) {
  signal(SIGINT, Handler);
  signal(SIGTERM, Handler);

  const char *override_ep = nullptr;
  for (int32_t i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--push") == 0 && i + 1 < argc) override_ep = argv[++i];
  }

  if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，使用内置默认值");

  std::string ep;
  if (override_ep) {
    ep = override_ep;
  } else {
    const std::string rk_ip = vox::config::get("rk.ip", "192.168.137.200");
    ep = "tcp://" + rk_ip + ":" + vox::config::get("rk.asr_port", "6711");
  }

  void *ctx = zmq_ctx_new();
  void *push = zmq_socket(ctx, ZMQ_PUSH);
  // 上游无人接收时本地最多积压 2s（32ms/包 ≈ 60 包），防内存膨胀；断链即丢
  int sndhwm = 64, linger = 0;
  zmq_setsockopt(push, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
  zmq_setsockopt(push, ZMQ_LINGER, &linger, sizeof(linger));
  if (zmq_connect(push, ep.c_str()) != 0) {
    VOX_ERROR("PUSH connect %s 失败: %s", ep.c_str(), zmq_strerror(zmq_errno()));
    return 1;
  }
  VOX_INFO("PUSH → %s（RK NPU ASR）", ep.c_str());

  if (Pa_Initialize() != paNoError) {
    VOX_ERROR("PortAudio 初始化失败");
    return 1;
  }

  int32_t device = paNoDevice;
  if (const char *env = getenv("SHERPA_ONNX_MIC_DEVICE")) {
    device = atoi(env);
  } else {
    std::string dev_str = vox::config::get("asr.mic_device", "");
    if (!dev_str.empty()) device = atoi(dev_str.c_str());
  }
  if (device == paNoDevice) device = Pa_GetDefaultInputDevice();

  PaStreamParameters param;
  memset(&param, 0, sizeof(param));
  param.device = device;
  param.channelCount = 1;
  param.sampleFormat = paFloat32;
  param.suggestedLatency =
      Pa_GetDeviceInfo(device)->defaultLowInputLatency;

  static constexpr int32_t kSamplesPerCallback = 512;  // 32ms @16k
  PaStream *stream = nullptr;
  PaError err = Pa_OpenStream(&stream, &param, nullptr, 16000,
                              kSamplesPerCallback, paClipOff, nullptr, nullptr);
  if (err != paNoError) {
    VOX_ERROR("Pa_OpenStream 失败: %s（device=%d，用 SHERPA_ONNX_MIC_DEVICE 指定）",
              Pa_GetErrorText(err), device);
    return 1;
  }
  Pa_StartStream(stream);
  VOX_INFO("麦克风采集开始（device=%d, 512 样本/回调 ≈ 32ms）", device);

  std::vector<float> buf(kSamplesPerCallback);
  while (!stop_) {
    err = Pa_ReadStream(stream, buf.data(), kSamplesPerCallback);
    if (err == paInputOverflowed) VOX_WARN("采集溢出（继续）");
    else if (err != paNoError) {
      VOX_ERROR("Pa_ReadStream: %s", Pa_GetErrorText(err));
      break;
    }
    if (zmq_send(push, buf.data(), kSamplesPerCallback * sizeof(float), ZMQ_DONTWAIT) == -1) {
      // RK 未启动/断链：丢弃本包继续采（重连由 ZMQ 自动完成）
      VOX_DEBUG("PUSH send 丢弃（%s）", zmq_strerror(zmq_errno()));
    }
  }

  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  zmq_close(push);
  zmq_ctx_destroy(ctx);
  VOX_INFO("mic_stream 退出");
  return 0;
}
