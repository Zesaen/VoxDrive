// asr — 流式语音识别（sherpa-onnx zipformer 双语 zh-en，PortAudio 麦克风）
//
// 基于 sherpa-onnx 官方 microphone 例程改造：
//   - 识别端点文本 → REQ :port.intent_router（经 conf，替代 ZmqClient 默认地址）
//   - 播报阻塞门 → REQ :port.tts_block（半双工防回灌，与 TTS 协议配套）
//   - 模型路径缺省取 conf model.asr_dir（int8 三件套）；命令行 --tokens 等参数仍可覆盖
//   - 麦克风：SHERPA_ONNX_MIC_DEVICE 环境变量 > conf asr.mic_device（PortAudio 设备号）> 默认
//
// Copyright (c)  2022-2023  Xiaomi Corporation（上游例程）
#define VOX_LOG_TAG "asr"
#include "vox_config.h"
#include "vox_log.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <clocale>
#include <cwctype>
#include <string>

#include "ZmqClient.h"
#include "portaudio.h"  // NOLINT
#include "sherpa-onnx/csrc/display.h"
#include "sherpa-onnx/csrc/microphone.h"
#include "sherpa-onnx/csrc/online-recognizer.h"

bool stop = false;
float mic_sample_rate = 16000;
bool wait = false;

static int32_t RecordCallback(const void *input_buffer,
                              void * /*output_buffer*/,
                              unsigned long frames_per_buffer,  // NOLINT
                              const PaStreamCallbackTimeInfo * /*time_info*/,
                              PaStreamCallbackFlags /*status_flags*/,
                              void *user_data) {
  if (!wait) {
    auto stream = reinterpret_cast<sherpa_onnx::OnlineStream *>(user_data);

    stream->AcceptWaveform(mic_sample_rate,
                           reinterpret_cast<const float *>(input_buffer),
                           frames_per_buffer);
  }

  return stop ? paComplete : paContinue;
}

static void Handler(int32_t /*sig*/) {
  stop = true;
  fprintf(stderr, "\nCaught Ctrl + C. Exiting...\n");
}

static std::string tolowerUnicode(const std::string &input_str) {
  // Use system locale
  std::setlocale(LC_ALL, "");

  // From char string to wchar string
  std::wstring input_wstr(input_str.size() + 1, '\0');
  std::mbstowcs(&input_wstr[0], input_str.c_str(), input_str.size());
  std::wstring lowercase_wstr;

  for (wchar_t wc : input_wstr) {
    if (std::iswupper(wc)) {
      lowercase_wstr += std::towlower(wc);
    } else {
      lowercase_wstr += wc;
    }
  }

  // Back to char string
  std::string lowercase_str(input_wstr.size() + 1, '\0');
  std::wcstombs(&lowercase_str[0], lowercase_wstr.c_str(),
                lowercase_wstr.size());

  return lowercase_str;
}

int32_t main(int32_t argc, char *argv[]) {
  signal(SIGINT, Handler);
  if (!vox::config::load())
    VOX_WARN("未找到 voxdrive.conf，端口/模型路径使用内置默认值");

  // 识别结果 → 路由、阻塞门 → TTS（端口走 conf）
  zmq_component::ZmqClient client(
      vox::config::connect_endpoint("port.intent_router", "6666"));
  zmq_component::ZmqClient block_client(
      vox::config::connect_endpoint("port.tts_block", "6677"));

  const char *kUsageMessage = R"usage(
This program uses streaming models with microphone for speech recognition.
Usage:

  ./sherpa-onnx-microphone \
    --tokens=/path/to/tokens.txt \
    --encoder=/path/to/encoder.onnx \
    --decoder=/path/to/decoder.onnx \
    --joiner=/path/to/joiner.onnx \
    --provider=cpu \
    --num-threads=1 \
    --decoding-method=greedy_search

模型参数缺省时自动取 voxdrive.conf 的 model.asr_dir（int8 三件套）。
)usage";

  sherpa_onnx::ParseOptions po(kUsageMessage);
  sherpa_onnx::OnlineRecognizerConfig config;

  config.Register(&po);
  po.Read(argc, argv);
  if (po.NumArgs() != 0) {
    po.PrintUsage();
    exit(EXIT_FAILURE);
  }

  // 模型路径缺省：conf model.asr_dir 下的 int8 三件套（见 models_manifest.md）
  {
    const std::string d = vox::config::get("model.asr_dir", "");
    auto &mc = config.model_config;
    if (!d.empty()) {
      if (mc.tokens.empty()) mc.tokens = d + "/tokens.txt";
      if (mc.transducer.encoder.empty())
        mc.transducer.encoder = d + "/encoder-epoch-99-avg-1.int8.onnx";
      if (mc.transducer.decoder.empty())
        mc.transducer.decoder = d + "/decoder-epoch-99-avg-1.int8.onnx";
      if (mc.transducer.joiner.empty())
        mc.transducer.joiner = d + "/joiner-epoch-99-avg-1.int8.onnx";
    }
  }

  fprintf(stderr, "%s\n", config.ToString().c_str());

  if (!config.Validate()) {
    fprintf(stderr, "Errors in config!\n");
    return -1;
  }

  sherpa_onnx::OnlineRecognizer recognizer(config);
  auto s = recognizer.CreateStream();

  sherpa_onnx::Microphone mic;

  PaDeviceIndex num_devices = Pa_GetDeviceCount();
  VOX_INFO("PortAudio 设备数: %d", num_devices);

  int32_t device_index = Pa_GetDefaultInputDevice();

  if (device_index == paNoDevice) {
    VOX_ERROR("无默认输入设备；Linux 下可用 SHERPA_ONNX_MIC_DEVICE 指定设备号");
    exit(EXIT_FAILURE);
  }

  // 麦克风选择：环境变量 > conf asr.mic_device > 系统默认
  if (const char *env = std::getenv("SHERPA_ONNX_MIC_DEVICE")) {
    VOX_INFO("使用环境变量指定设备: %s", env);
    device_index = atoi(env);
  } else {
    int conf_dev = vox::config::get_int("asr.mic_device", -1);
    if (conf_dev >= 0) device_index = conf_dev;
  }

  for (int32_t i = 0; i != num_devices; ++i) {
    const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
    VOX_DEBUG(" %s %d %s", (i == device_index) ? "*" : " ", i, info->name);
  }

  PaStreamParameters param;
  param.device = device_index;

  const PaDeviceInfo *info = Pa_GetDeviceInfo(param.device);
  VOX_INFO("麦克风: #%d %s（%d 通道）", param.device, info->name,
           info->maxInputChannels);

  param.channelCount = 1;
  param.sampleFormat = paFloat32;

  param.suggestedLatency = info->defaultLowInputLatency;
  param.hostApiSpecificStreamInfo = nullptr;

  mic_sample_rate = vox::config::get_int("asr.sample_rate", 16000);
  if (const char *env = std::getenv("SHERPA_ONNX_MIC_SAMPLE_RATE")) {
    mic_sample_rate = atof(env);
    VOX_INFO("使用环境变量采样率: %f", mic_sample_rate);
  }
  float sample_rate = 16000;

  PaStream *stream;
  PaError err =
      Pa_OpenStream(&stream, &param, nullptr, /* &outputParameters, */
                    sample_rate,
                    0,          // frames per buffer
                    paClipOff,  // we won't output out of range samples
                                // so don't bother clipping them
                    RecordCallback, s.get());
  if (err != paNoError) {
    VOX_ERROR("portaudio error: %s", Pa_GetErrorText(err));
    exit(EXIT_FAILURE);
  }

  err = Pa_StartStream(stream);
  if (err != paNoError) {
    VOX_ERROR("portaudio error: %s", Pa_GetErrorText(err));
    exit(EXIT_FAILURE);
  }
  VOX_INFO("麦克风流已启动，进入识别循环");

  std::string last_text;
  int32_t segment_index = 0;
  sherpa_onnx::Display display(30);
  while (!stop) {
    while (recognizer.IsReady(s.get())) {
      recognizer.DecodeStream(s.get());
    }

    auto text = recognizer.GetResult(s.get()).text;
    bool is_endpoint = recognizer.IsEndpoint(s.get());

    if (is_endpoint && !config.model_config.paraformer.encoder.empty()) {
      // For streaming paraformer models, since it has a large right chunk size
      // we need to pad it on endpointing so that the last character
      // can be recognized
      std::vector<float> tail_paddings(static_cast<int>(1.0 * mic_sample_rate));
      s->AcceptWaveform(mic_sample_rate, tail_paddings.data(),
                        tail_paddings.size());
      while (recognizer.IsReady(s.get())) {
        recognizer.DecodeStream(s.get());
      }
      text = recognizer.GetResult(s.get()).text;
    }

    if (!text.empty() && last_text != text) {
      last_text = text;

      display.Print(segment_index, tolowerUnicode(text));
      fflush(stderr);
    }

    if (is_endpoint) {
      if (!text.empty()) {
        // 端点检测到一句话结束 → 送路由；随后置阻塞门等 TTS 播报（半双工防回灌）
        auto response = client.request(text);
        VOX_INFO("[router ->] %.80s", response.c_str());

        wait = true;
        auto block_response = block_client.request("block");
        VOX_INFO("[tts block ->] %.30s", block_response.c_str());
        wait = false;

        ++segment_index;
      }

      recognizer.Reset(s.get());
    }

    Pa_Sleep(20);  // sleep for 20ms
  }

  err = Pa_CloseStream(stream);
  if (err != paNoError) {
    VOX_ERROR("portaudio error: %s", Pa_GetErrorText(err));
    exit(EXIT_FAILURE);
  }

  return 0;
}
