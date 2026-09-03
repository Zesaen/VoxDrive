// recorder_service.cpp — RK 记录仪守护进程（C5/R5）
//
// 结构：
//   管线线程：V4L2Capture → MppEncoder → sinks 扇出（当前 Mp4SegmentSink，C4 加 RtmpSink）
//             事件（分段/水位/写失败/断流）经 ZmqPub 上行（消息信封 type=event）
//   控制线程：ZmqServer REP 应答状态查询/录像开关（信封 type=status）
// 协议（跨板，见 jetson/common/msg_envelope.h）：
//   REQ 体=信封{type:"status", payload:{"cmd":"status"|"set_recording","value":bool}}
//   REP 体=信封{type:"status", payload:{recording, pipeline_fps, storage{...}, ...}}
// 用法：recorder_service [--seconds N] [--segment-seconds s] [--dir D]（缺省走 voxdrive.conf）
#define VOX_LOG_TAG "rk.recorder"

#include <signal.h>
#include <sys/statvfs.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ZmqPub.h"
#include "ZmqServer.h"
#include "json.hpp"
#include "mpp_encoder.h"
#include "mp4_segment_sink.h"
#include "rtmp_sink.h"
#include "v4l2_capture.h"
#include "msg_envelope.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

volatile sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

struct SharedState {
  std::mutex mu;
  bool recording = true;          // 录像开关（暂停=编码继续但不落盘，预览流不受影响）
  double pipeline_fps = 0.0;      // EWMA
  uint64_t frames_encoded = 0;
  uint64_t frames_dropped = 0;    // 暂停期间丢弃的编码帧
  int64_t start_ns = 0;
};

int64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 存储快照（水位/剩余空间）——状态查询与事件共用
nlohmann::json storage_snapshot(const std::string& dir, const vox::Mp4SegmentSink& sink) {
  const auto st = sink.stats_snapshot();  // 线程安全快照
  nlohmann::json j{{"dir", dir},
                   {"segments_total", st.segments_written},
                   {"segments_deleted", st.segments_deleted},
                   {"bytes_written", st.bytes_written},
                   {"current_file", st.current_file}};
  struct statvfs vfs {};
  if (statvfs(dir.c_str(), &vfs) == 0 && vfs.f_blocks > 0) {
    j["used_percent"] = 100.0 * (1.0 - static_cast<double>(vfs.f_bavail) / vfs.f_blocks);
    j["free_gb"] = static_cast<double>(vfs.f_bavail) * vfs.f_bsize / 1e9;
  }
  return j;
}

nlohmann::json rtmp_snapshot(const vox::RtmpSink& sink) {
  const auto st = sink.stats_snapshot();
  return nlohmann::json{{"connected", st.connected},
                        {"frames_sent", st.frames_sent},
                        {"connect_count", st.connect_count},
                        {"connect_failures", st.connect_failures},
                        {"write_errors", st.write_errors}};
}

}  // namespace

int main(int argc, char** argv) {
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  double run_seconds = 0;  // 0=常驻
  std::string dir, rtmp_url;
  uint32_t seg_seconds = 0, watermark = 0;
  for (int i = 1; i < argc; ++i) {
    std::string s(argv[i]);
    if (s == "--seconds" && i + 1 < argc) run_seconds = std::stod(argv[++i]);
    else if (s == "--segment-seconds" && i + 1 < argc) seg_seconds = std::stoi(argv[++i]);
    else if (s == "--dir" && i + 1 < argc) dir = argv[++i];
    else if (s == "--watermark" && i + 1 < argc) watermark = std::stoi(argv[++i]);
    else if (s == "--rtmp-url" && i + 1 < argc) rtmp_url = argv[++i];
    else {
      VOX_ERROR("未知参数 %s", s.c_str());
      return 2;
    }
  }
  if (!vox::config::load()) {
    std::printf("RECORDER_SERVICE FAIL conf\n");
    return 1;
  }

  // ---- 采集/编码/存储参数（conf 缺省 + 命令行覆盖）----
  vox::V4L2Capture::Params cp;
  cp.device = vox::config::get("rk.video_device", "/dev/video11");
  cp.width = vox::config::get_int("rk.capture_width", 1920);
  cp.height = vox::config::get_int("rk.capture_height", 1080);
  cp.fps = vox::config::get_int("rk.capture_fps", 30);
  cp.buffer_count = vox::config::get_int("rk.capture_buffers", 4);

  const std::string storage_dir = !dir.empty()
                                      ? dir
                                      : vox::config::get("rk.storage_dir", "$HOME/voxdrive_records");
  vox::Mp4SegmentSink::Params sp;
  sp.dir = storage_dir;
  sp.segment_seconds =
      seg_seconds ? seg_seconds : vox::config::get_int("rk.segment_seconds", 60);
  sp.watermark_percent =
      watermark ? watermark : vox::config::get_int("rk.watermark_percent", 85);
  sp.fps = cp.fps;

  const std::string status_endpoint =
      vox::config::bind_endpoint("rk.status_port", "6700");
  const std::string event_endpoint =
      vox::config::bind_endpoint("rk.event_port", "6701");

  SharedState st;
  st.start_ns = steady_ns();

  // ---- 事件上行 PUB（仅管线线程使用）----
  zmq_component::ZmqPub event_pub(event_endpoint);

  vox::V4L2Capture cap(cp);
  vox::MppEncoder enc;
  vox::Mp4SegmentSink sink(sp);
  sink.set_event_handler([&event_pub](const char* event, const std::string& detail) {
    nlohmann::json payload{{"event", event}};
    payload["detail"] = nlohmann::json::parse(detail, nullptr, false);
    event_pub.publish(vox::msg::make(vox::msg::kTypeEvent, "rk.recorder", payload).dump());
  });

  // ---- 推流 sink（C4/R4）：conf rk.rtmp_url 或 --rtmp-url 指定，空=不推流 ----
  if (rtmp_url.empty()) rtmp_url = vox::config::get("rk.rtmp_url", "");
  std::unique_ptr<vox::RtmpSink> rtmp;
  if (!rtmp_url.empty()) {
    vox::RtmpSink::Params rp;
    rp.url = rtmp_url;
    rp.width = cp.width;
    rp.height = cp.height;
    rp.fps = cp.fps;
    rtmp = std::make_unique<vox::RtmpSink>(rp);
    rtmp->set_event_handler([&event_pub](const char* event, const std::string& detail) {
      nlohmann::json payload{{"event", event}};
      payload["detail"] = nlohmann::json::parse(detail, nullptr, false);
      event_pub.publish(vox::msg::make(vox::msg::kTypeEvent, "rk.recorder", payload).dump());
    });
  }

  std::atomic<bool> pipeline_ok{false};
  std::atomic<bool> pipeline_done{false};

  // ---- 管线线程 ----
  std::thread pipeline([&]() {
    struct DoneFlag {  // 任意退出路径（含启动失败）都标记结束
      std::atomic<bool>& f;
      ~DoneFlag() { f.store(true); }
    } done{pipeline_done};
    if (!cap.start()) return;
    vox::MppEncoder::Params ep;
    ep.width = cap.width();
    ep.height = cap.height();
    ep.stride = cap.stride();
    ep.fps = cp.fps;
    ep.gop = cp.fps * vox::config::get_int("rk.gop_seconds", 2);
    ep.bitrate_bps = vox::config::get_int("rk.bitrate_bps", 4000000);
    if (!enc.start(ep)) {
      cap.stop();
      return;
    }
    if (!sink.start(enc.sps_pps())) {
      cap.stop();
      return;
    }
    if (rtmp && !rtmp->start(enc.sps_pps())) {
      VOX_WARN("RTMP sink 初始化失败，仅录像（推流不可用不影响录制）");
      rtmp.reset();  // 失败只发生在服务就绪前，控制线程尚未应答，无并发
    }
    pipeline_ok.store(true);
    VOX_INFO("pipeline up: %s", cap.describe().c_str());

    int64_t last_ns = 0;
    int capture_timeouts = 0;
    while (g_stop == 0) {
      if (run_seconds > 0 &&
          (steady_ns() - st.start_ns) / 1e9 >= run_seconds) {
        break;
      }
      const vox::VideoFrame* f = cap.acquire(2000);
      if (!f) {
        if (++capture_timeouts == 1) {  // 事件只发首次，避免风暴
          event_pub.publish(vox::msg::make(vox::msg::kTypeEvent, "rk.recorder",
                                           {{"event", "capture_timeout"}})
                                .dump());
        }
        continue;
      }
      capture_timeouts = 0;
      if (last_ns > 0) {
        const double inst = 1e9 / static_cast<double>(f->timestamp_ns - last_ns);
        {
          std::lock_guard<std::mutex> lk(st.mu);
          if (st.pipeline_fps == 0.0) st.pipeline_fps = inst;
          st.pipeline_fps = st.pipeline_fps * 0.9 + inst * 0.1;
        }
      }
      last_ns = f->timestamp_ns;

      bool record_this = false;
      {
        std::lock_guard<std::mutex> lk(st.mu);
        record_this = st.recording;
      }
      const vox::EncodedPacket* pkt = nullptr;
      if (enc.encode(*f, &pkt) && pkt) {
        if (record_this) sink.on_packet(*pkt);
        else {
          std::lock_guard<std::mutex> lk(st.mu);
          st.frames_dropped++;
        }
        if (rtmp) rtmp->on_packet(*pkt);  // 预览流不受录像开关影响
        std::lock_guard<std::mutex> lk(st.mu);
        st.frames_encoded++;
      }
      cap.release(f);
    }
    sink.stop();  // 收尾当前段（写 trailer）
    if (rtmp) rtmp->stop();
    cap.stop();
    VOX_INFO("pipeline down");
  });

  // 等管线就绪或启动失败（打开设备/初始化编码器需时，不能即时判定）
  while (!pipeline_ok.load() && !pipeline_done.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  if (!pipeline_ok.load()) {
    pipeline.join();
    std::printf("RECORDER_SERVICE FAIL pipeline\n");
    return 1;
  }

  // ---- 控制线程（REP 状态/控制应答）----
  zmq_component::ZmqServer server(status_endpoint);
  server.setTimeout(500);
  VOX_INFO("service up: status=%s event=%s", status_endpoint.c_str(),
           event_endpoint.c_str());

  while (g_stop == 0) {
    if (run_seconds > 0 && (steady_ns() - st.start_ns) / 1e9 >= run_seconds) break;
    std::string req;
    try {
      req = server.receive();
    } catch (const zmq_component::ZmqCommunicationError&) {
      continue;  // 轮询超时
    }
    nlohmann::json env = nlohmann::json::parse(req, nullptr, false);
    nlohmann::json payload =
        (vox::msg::valid(env) && env["payload"].is_object()) ? env["payload"]
                                                             : nlohmann::json::object();
    const std::string cmd = payload.value("cmd", "");

    nlohmann::json reply_payload;
    if (cmd == "status") {
      std::lock_guard<std::mutex> lk(st.mu);
      reply_payload = {
          {"recording", st.recording},
          {"pipeline_fps", st.pipeline_fps},
          {"frames_encoded", st.frames_encoded},
          {"frames_dropped", st.frames_dropped},
          {"uptime_s", (steady_ns() - st.start_ns) / 1e9},
          {"storage", storage_snapshot(storage_dir, sink)}};
      if (rtmp) reply_payload["rtmp"] = rtmp_snapshot(*rtmp);
    } else if (cmd == "set_recording") {
      const bool want = payload.value("value", true);
      {
        std::lock_guard<std::mutex> lk(st.mu);
        st.recording = want;
      }
      VOX_INFO("录像开关 → %s", want ? "on" : "off");
      reply_payload = {{"recording", want}};
    } else {
      reply_payload = {{"error", "unknown cmd"}};
    }
    server.send(vox::msg::make(vox::msg::kTypeStatus, "rk.recorder", reply_payload).dump());
  }

  pipeline.join();
  VOX_INFO("recorder service exit");
  return 0;
}
