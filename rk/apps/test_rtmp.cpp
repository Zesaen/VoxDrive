// test_rtmp.cpp — C4/R4 推流自测
//
// 回环验证（板上无需 RTMP 服务器软件）：ffmpeg 原生 rtmp 协议支持 listen 模式，
// 用它做单连接接收端，把推流落成文件再 ffprobe/ffmpeg 校验。
//
// 相位 A（正常路径）：监听端先起 → 推 8s → 拉流文件校验（编码参数/时长/解码零错误）
// 相位 B（断链恢复）：监听端 3s 处主动断开（-t 3 停收）→ 推流端应检测写失败、
//   走"I 帧+冷却"重连（后续重连会失败，因为监听端已退出）→ 管线必须跑满全程不崩。
// 判据（输出末行 RTMP_PUSH_TEST PASS/FAIL）：
//   A: 拉流 h264 1920x1080、时长 ≥6s、ffmpeg 全量解码 stderr 为空
//   B: 管线帧数达标（30fps×8s±15%）、断链后有 ≥1 次写失败/重连尝试、进程正常退出
// 用法：test_rtmp [port]（默认 21935，避开标准 1935 防与未来服务冲突）
#define VOX_LOG_TAG "rk.test_rtmp"

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "mpp_encoder.h"
#include "rtmp_sink.h"
#include "v4l2_capture.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

int run_popen(const std::string& cmd, std::string& out) {
  FILE* f = popen((cmd + " 2>&1").c_str(), "r");
  if (!f) return -1;
  char buf[512];
  while (fgets(buf, sizeof(buf), f)) out += buf;
  const int rc = pclose(f);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

// pclose/-1 都算失败；ffmpeg -v error 时 stderr 空 = 解码干净
bool ffmpeg_decode_clean(const std::string& file) {
  std::string out;
  const int rc = run_popen("ffmpeg -v error -i " + file + " -f null -", out);
  return rc == 0 && out.empty();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "21935";
  const std::string url = "rtmp://127.0.0.1:" + port + "/live/vox";
  const std::string pulled = "/tmp/vox_rtmp_pulled.flv";
  if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，使用内置默认值");

  vox::V4L2Capture::Params cp;
  cp.device = vox::config::get("rk.video_device", "/dev/video11");
  cp.width = vox::config::get_int("rk.capture_width", 1920);
  cp.height = vox::config::get_int("rk.capture_height", 1080);
  cp.fps = vox::config::get_int("rk.capture_fps", 30);
  cp.buffer_count = vox::config::get_int("rk.capture_buffers", 4);

  // ---- 相位 A：先起监听端，再推 8s ----
  std::remove(pulled.c_str());
  // -listen 1 原生 rtmp 服务模式；外层 timeout 25s 防监听端异常时永久挂住
  const std::string listener = "timeout 25 ffmpeg -hide_banner -loglevel warning "
                               "-rw_timeout 15000000 -listen 1 -i " + url +
                               " -c copy -y " + pulled;
  system(("nohup " + listener + " > /tmp/vox_rtmp_listener.log 2>&1 &").c_str());
  std::this_thread::sleep_for(std::chrono::seconds(2));  // 监听就绪

  vox::RtmpSink::Params rp;
  rp.url = url;
  rp.width = cp.width;
  rp.height = cp.height;
  rp.fps = cp.fps;

  vox::V4L2Capture cap(cp);
  vox::MppEncoder enc;
  vox::RtmpSink sink(rp);
  int rtmp_events = 0;
  sink.set_event_handler([&rtmp_events](const char* event, const std::string&) {
    rtmp_events++;
    VOX_INFO("event: %s", event);
  });

  const int push_seconds = 8;
  bool phase_a_ok = false;
  uint64_t frames_total = 0;
  {
    if (!cap.start()) { std::printf("RTMP_PUSH_TEST FAIL capture\n"); return 1; }
    vox::MppEncoder::Params ep;
    ep.width = cap.width();
    ep.height = cap.height();
    ep.stride = cap.stride();
    ep.fps = cp.fps;
    ep.gop = cp.fps * vox::config::get_int("rk.gop_seconds", 2);
    ep.bitrate_bps = vox::config::get_int("rk.bitrate_bps", 4000000);
    if (!enc.start(ep)) { cap.stop(); std::printf("RTMP_PUSH_TEST FAIL encoder\n"); return 1; }
    if (!sink.start(enc.sps_pps())) { cap.stop(); std::printf("RTMP_PUSH_TEST FAIL sink\n"); return 1; }

    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() <
           push_seconds) {
      const vox::VideoFrame* f = cap.acquire(2000);
      if (!f) continue;
      const vox::EncodedPacket* pkt = nullptr;
      if (enc.encode(*f, &pkt) && pkt) {
        sink.on_packet(*pkt);
        frames_total++;
      }
      cap.release(f);
    }
    sink.stop();
    enc.stop();
    cap.stop();
  }
  std::this_thread::sleep_for(std::chrono::seconds(3));  // 等监听端收尾退出

  {
    std::string sparam, sdur;
    run_popen("ffprobe -v error -select_streams v:0 -show_entries "
              "stream=codec_name,width,height -of csv=p=0 " + pulled, sparam);
    run_popen("ffprobe -v error -show_entries format=duration -of csv=p=0 " + pulled, sdur);
    const double dur = atof(sdur.c_str());
    const bool params_ok = sparam.find("h264,1920,1080") != std::string::npos;
    const bool clean = ffmpeg_decode_clean(pulled);
    phase_a_ok = params_ok && dur >= push_seconds - 2.0 && clean;
    VOX_INFO("相位A: probe='%s' dur=%.2f 解码干净=%d", sparam.c_str(), dur, clean ? 1 : 0);
  }
  const auto st = sink.stats_snapshot();
  VOX_INFO("推流统计: frames=%llu 建连=%llu 连接失败=%llu 写失败=%llu 事件=%d",
           static_cast<unsigned long long>(st.frames_sent),
           static_cast<unsigned long long>(st.connect_count),
           static_cast<unsigned long long>(st.connect_failures),
           static_cast<unsigned long long>(st.write_errors), rtmp_events);

  // ---- 相位 B：监听端 3s 断开（-t 3），推流端须跑满 8s 不崩 ----
  bool phase_b_ok = false;
  {
    std::remove(pulled.c_str());
    const std::string listener_b = "timeout 25 ffmpeg -hide_banner -loglevel warning "
                                   "-rw_timeout 15000000 -listen 1 -i " + url +
                                   " -c copy -t 3 -y " + pulled;
    system(("nohup " + listener_b + " > /tmp/vox_rtmp_listener.log 2>&1 &").c_str());
    std::this_thread::sleep_for(std::chrono::seconds(2));

    vox::RtmpSink sink_b(rp);
    sink_b.set_event_handler([](const char* event, const std::string&) {
      VOX_INFO("B event: %s", event);
    });
    if (!cap.start()) { std::printf("RTMP_PUSH_TEST FAIL capture_b\n"); return 1; }
    vox::MppEncoder::Params ep;
    ep.width = cap.width();
    ep.height = cap.height();
    ep.stride = cap.stride();
    ep.fps = cp.fps;
    ep.gop = cp.fps * vox::config::get_int("rk.gop_seconds", 2);
    ep.bitrate_bps = vox::config::get_int("rk.bitrate_bps", 4000000);
    if (!enc.start(ep)) { cap.stop(); std::printf("RTMP_PUSH_TEST FAIL encoder_b\n"); return 1; }
    if (!sink_b.start(enc.sps_pps())) { cap.stop(); std::printf("RTMP_PUSH_TEST FAIL sink_b\n"); return 1; }

    uint64_t frames_b = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() <
           push_seconds) {
      const vox::VideoFrame* f = cap.acquire(2000);
      if (!f) continue;
      const vox::EncodedPacket* pkt = nullptr;
      if (enc.encode(*f, &pkt) && pkt) {
        sink_b.on_packet(*pkt);
        frames_b++;
      }
      cap.release(f);
    }
    sink_b.stop();
    enc.stop();
    cap.stop();
    const auto sb = sink_b.stats_snapshot();
    const bool frames_ok = frames_b >= cp.fps * push_seconds * 0.85;
    const bool recovered = sb.write_errors >= 1 && sb.connect_failures >= 1;
    phase_b_ok = frames_ok && recovered;
    VOX_INFO("相位B: frames=%llu(需≥%d) 写失败=%llu 重连失败=%llu",
             static_cast<unsigned long long>(frames_b),
             static_cast<int>(cp.fps * push_seconds * 0.85),
             static_cast<unsigned long long>(sb.write_errors),
             static_cast<unsigned long long>(sb.connect_failures));
  }

  const bool pass = phase_a_ok && phase_b_ok;
  std::printf("RTMP_PUSH_TEST %s phase_a=%d phase_b=%d frames=%llu\n",
              pass ? "PASS" : "FAIL", phase_a_ok ? 1 : 0, phase_b_ok ? 1 : 0,
              static_cast<unsigned long long>(frames_total));
  return pass ? 0 : 1;
}
