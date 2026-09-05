// recorder_service.cpp — RK 记录仪守护进程（C5/R5）
//
// 结构：
//   管线线程：V4L2Capture → MppEncoder → sinks 扇出（当前 Mp4SegmentSink，C4 加 RtmpSink）
//             事件（分段/水位/写失败/断流）经 ZmqPub 上行（消息信封 type=event）
//   控制线程：ZmqServer REP 应答状态查询/录像开关（信封 type=status）
// 协议（跨板，见 jetson/common/msg_envelope.h）：
//   REQ 体=信封{type:"status", payload:{"cmd":"status"|"set_recording"|"set_preview"|"snapshot", ...}}
//   REP 体=信封{type:"status"|...， payload:{recording, pipeline_fps, storage{...}, ...}}
//   snapshot 应答信封 type="snapshot"，payload 携带 jpeg_b64（D1/R6 抓拍）
// 用法：recorder_service [--seconds N] [--segment-seconds s] [--dir D]（缺省走 voxdrive.conf）
#define VOX_LOG_TAG "rk.recorder"

#include <signal.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "ZmqPub.h"
#include "ZmqServer.h"
#include "jpeg_encoder.h"
#include "json.hpp"
#include "mpp_encoder.h"
#include "mp4_segment_sink.h"
#include "rknn_detector.h"
#include "rtmp_sink.h"
#include "v4l2_capture.h"
#include "base64.h"
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

// 检测线程统计快照（检测线程写，控制线程读）
struct DetectState {
  std::mutex mu;
  bool enabled = false;
  uint64_t inferences = 0;
  uint64_t detections = 0;        // 过滤后命中目标数
  uint64_t lock_events = 0;       // 触发锁段的事件次数
  double infer_ms_ewma = 0.0;
  double infer_ms_max = 0.0;
};

// 最新帧槽（R10）：管线线程每 N 帧塞入紧凑 NV12 拷贝，检测线程取走。
// 检测线程忙时旧帧被覆盖（最新帧语义，检测不需要每一帧）。
struct DetectSlot {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> nv12;
  int w = 0, h = 0;
  bool fresh = false;
};

// 检测事件邮箱：检测线程投递 JSON（信封），管线线程发布——
// zmq socket 非线程安全，PUB 保持在管线线程单线程使用。
struct EventMailbox {
  std::mutex mu;
  std::string envelope;
  bool pending = false;
};

// 抓拍请求：控制线程发起 → 管线线程在下一帧编码 JPEG → 控制线程取走应答
struct SnapshotRequest {
  std::mutex mu;
  std::condition_variable cv;
  bool pending = false;           // 置位表示请求在途（管线线程独占清零）
  bool done = false;
  std::vector<uint8_t> jpeg;
};

// NV12 带行距 → 紧凑拷贝（检测线程持有帧期间采集缓冲已归还，必须拷贝）
void pack_nv12(const vox::VideoFrame* f, std::vector<uint8_t>& out) {
  const int w = static_cast<int>(f->width), h = static_cast<int>(f->height);
  out.resize(static_cast<size_t>(w) * h * 3 / 2);
  const uint8_t* y = static_cast<const uint8_t*>(f->plane[0]);
  const uint8_t* uv = static_cast<const uint8_t*>(f->plane[1]);
  const size_t sy = f->plane_stride[0], suv = f->plane_stride[1];
  uint8_t* d = out.data();
  for (int r = 0; r < h; ++r) memcpy(d + static_cast<size_t>(r) * w, y + r * sy, w);
  uint8_t* duv = d + static_cast<size_t>(w) * h;
  for (int r = 0; r < h / 2; ++r) memcpy(duv + static_cast<size_t>(r) * w, uv + r * suv, w);
}

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
                   {"locked_segments", st.locked_segments},
                   {"locked_bytes", st.locked_bytes},
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
  sp.lock_quota_bytes =
      static_cast<uint64_t>(vox::config::get_int("rk.lock_quota_mb", 500)) * 1024 * 1024;
  sp.fps = cp.fps;

  const std::string status_endpoint =
      vox::config::bind_endpoint("rk.status_port", "6700");
  const std::string event_endpoint =
      vox::config::bind_endpoint("rk.event_port", "6701");

  SharedState st;
  st.start_ns = steady_ns();
  SnapshotRequest snap;
  DetectState dst;
  DetectSlot dslot;
  EventMailbox detect_mailbox;

  // ---- 事件锁录检测器（R10）：conf rk.detect_model 缺失/文件不存在 → 优雅禁用 ----
  const std::string detect_model =
      vox::config::get("rk.detect_model", "$HOME/Desktop/VoxDrive/models/yolov5s-640-640.rknn");
  vox::RknnDetector detector;
  const int detect_interval = vox::config::get_int("rk.detect_interval_frames", 30);  // 30帧=1fps
  const int detect_cooldown_ms = vox::config::get_int("rk.detect_cooldown_s", 10) * 1000;
  std::set<int> detect_classes;  // COCO: 0=person 2=car 3=motorcycle 5=bus 7=truck
  {
    std::string cs = vox::config::get("rk.detect_classes", "0,2,3,5,7");
    for (char* tok = std::strtok(cs.data(), ", "); tok; tok = std::strtok(nullptr, ", "))
      detect_classes.insert(std::atoi(tok));
  }

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

  // ---- 事件锁录：检测器初始化 + 检测线程（R10）----
  vox::RknnDetector::Params dp;
  dp.model_path = detect_model;
  dp.target_classes = detect_classes;
  dp.box_thresh = static_cast<float>(vox::config::get_double("rk.detect_box_thresh", 0.25));
  bool detect_on = false;
  if (access(detect_model.c_str(), R_OK) == 0) {
    detect_on = detector.start(dp);
    if (detect_on) {
      {
        std::lock_guard<std::mutex> lk(dst.mu);
        dst.enabled = true;
      }
      VOX_INFO("事件锁录启用: %s 间隔 %d 帧 冷却 %dms", detect_model.c_str(),
               detect_interval, detect_cooldown_ms);
    } else {
      VOX_WARN("检测器初始化失败（%s），事件锁录禁用，录像不受影响",
               detector.error().c_str());
    }
  } else {
    VOX_WARN("检测模型不存在（%s），事件锁录禁用，录像不受影响", detect_model.c_str());
  }

  std::thread detect_thread;
  // 检测线程在管线就绪判定之后启动（失败早退路径不创建，避免未 join 线程析构）

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

      // 抓拍：在编码前对原始 NV12 帧出 JPEG（录像暂停期间同样可用）
      {
        std::lock_guard<std::mutex> lk(snap.mu);
        if (snap.pending) {
          snap.jpeg = vox::encode_jpeg_nv12(
              static_cast<const uint8_t*>(f->plane[0]),
              static_cast<const uint8_t*>(f->plane[1]), f->width, f->height,
              static_cast<int>(f->plane_stride[0]),
              static_cast<int>(f->plane_stride[1]));
          snap.pending = false;
          snap.done = true;
          snap.cv.notify_one();
        }
      }

      if (last_ns > 0) {
        const double inst = 1e9 / static_cast<double>(f->timestamp_ns - last_ns);
        {
          std::lock_guard<std::mutex> lk(st.mu);
          if (st.pipeline_fps == 0.0) st.pipeline_fps = inst;
          st.pipeline_fps = st.pipeline_fps * 0.9 + inst * 0.1;
        }
      }
      last_ns = f->timestamp_ns;

      // 事件锁录取帧（R10）：每 N 帧塞最新帧槽，检测线程异步消费（不阻塞主管线）
      if (detect_on && f->sequence % detect_interval == 0) {
        {
          std::lock_guard<std::mutex> lk(dslot.mu);
          pack_nv12(f, dslot.nv12);
          dslot.w = static_cast<int>(f->width);
          dslot.h = static_cast<int>(f->height);
          dslot.fresh = true;
        }
        dslot.cv.notify_one();
      }
      // 检测事件邮箱：zmq socket 单线程约束，由管线线程统一发布
      {
        std::lock_guard<std::mutex> lk(detect_mailbox.mu);
        if (detect_mailbox.pending) {
          event_pub.publish(detect_mailbox.envelope);
          detect_mailbox.pending = false;
        }
      }

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
        if (rtmp) rtmp->on_packet(*pkt);  // 预览开关在 RtmpSink 内部处理（关=断流，开=I 帧自动重连）
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

  // ---- 检测线程（R10）：管线就绪后启动，失败早退路径不会走到这里 ----
  if (detect_on) {
    detect_thread = std::thread([&]() {
      int64_t last_event_ns = 0;
      while (g_stop == 0) {
        std::vector<uint8_t> buf;
        int w = 0, h = 0;
        {
          std::unique_lock<std::mutex> lk(dslot.mu);
          dslot.cv.wait(lk, [&] { return dslot.fresh || g_stop; });
          if (g_stop) break;
          buf = std::move(dslot.nv12);
          dslot.nv12.clear();
          dslot.nv12.shrink_to_fit();
          w = dslot.w;
          h = dslot.h;
          dslot.fresh = false;
        }
        std::vector<vox::Detection> dets = detector.detect_nv12(buf.data(), w, h);
        const auto& ds = detector.stats();  // 检测线程独占 detector，直读安全
        {
          std::lock_guard<std::mutex> lk(dst.mu);
          dst.inferences = ds.inferences;
          dst.detections = ds.detections;
          dst.infer_ms_ewma = ds.infer_ms_ewma;
          dst.infer_ms_max = ds.infer_ms_max;
        }
        if (dets.empty()) continue;
        const int64_t now = steady_ns();
        if (last_event_ns > 0 && now - last_event_ns < detect_cooldown_ms * 1000000ll)
          continue;  // 冷却：目标持续在画面内不重复锁段/刷事件
        last_event_ns = now;

        nlohmann::json objs = nlohmann::json::array();
        for (const auto& d : dets)
          objs.push_back({{"cls", d.cls}, {"prop", d.prop},
                          {"box", {d.left, d.top, d.right, d.bottom}}});
        sink.lock_current("detect");
        {
          std::lock_guard<std::mutex> lk(dst.mu);
          dst.lock_events++;
        }
        nlohmann::json payload{{"event", "detect"},
                                {"detections", objs},
                                {"infer_ms", ds.infer_ms_ewma},
                                {"locked", true}};
        // 经邮箱交管线线程发布（zmq socket 非线程安全）
        {
          std::lock_guard<std::mutex> lk(detect_mailbox.mu);
          detect_mailbox.envelope =
              vox::msg::make(vox::msg::kTypeEvent, "rk.recorder", payload).dump();
          detect_mailbox.pending = true;
        }
        VOX_INFO("[detect] %zu 目标（首类 %d conf %.2f），锁定当前段",
                 dets.size(), dets.front().cls, dets.front().prop);
      }
    });
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
      if (rtmp) {
        reply_payload["rtmp"] = rtmp_snapshot(*rtmp);
        reply_payload["preview"] = rtmp->enabled();
      } else {
        reply_payload["preview"] = false;
      }
      {  // 事件锁录（R10）
        std::lock_guard<std::mutex> lk(dst.mu);
        nlohmann::json classes = nlohmann::json::array();
        for (int c : detect_classes) classes.push_back(c);
        reply_payload["detect"] = {
            {"enabled", dst.enabled},
            {"model", detect_model},
            {"classes", classes},
            {"inferences", dst.inferences},
            {"detections", dst.detections},
            {"lock_events", dst.lock_events},
            {"infer_ms", {{"ewma", dst.infer_ms_ewma}, {"max", dst.infer_ms_max}}}};
      }
    } else if (cmd == "set_recording") {
      const bool want = payload.value("value", true);
      {
        std::lock_guard<std::mutex> lk(st.mu);
        st.recording = want;
      }
      VOX_INFO("录像开关 → %s", want ? "on" : "off");
      reply_payload = {{"recording", want}};
    } else if (cmd == "set_preview") {
      const bool want = payload.value("value", true);
      if (rtmp) rtmp->set_enabled(want);
      VOX_INFO("预览开关 → %s（rtmp=%s）", want ? "on" : "off", rtmp ? "yes" : "no");
      reply_payload = {{"preview", rtmp ? rtmp->enabled() : false}};
    } else if (cmd == "snapshot") {
      // 置请求 → 管线线程下一帧出 JPEG（3s 内）
      std::unique_lock<std::mutex> lk(snap.mu);
      snap.jpeg.clear();
      snap.done = false;
      snap.pending = true;
      const bool got = snap.cv.wait_for(lk, std::chrono::seconds(3),
                                        [&] { return snap.done; });
      if (got && !snap.jpeg.empty() && snap.jpeg.size() > 2 &&
          snap.jpeg[0] == 0xff && snap.jpeg[1] == 0xd8) {
        reply_payload = {{"width", cp.width},
                         {"height", cp.height},
                         {"bytes", snap.jpeg.size()},
                         {"jpeg_b64", vox::b64_encode(snap.jpeg.data(), snap.jpeg.size())}};
        VOX_INFO("抓拍完成: %zu 字节 JPEG", snap.jpeg.size());
      } else {
        reply_payload = {{"error", got ? "jpeg encode failed" : "pipeline timeout"}};
      }
      // snapshot 应答走独立消息类型（信封注册表 kTypeSnapshot）
    } else {
      reply_payload = {{"error", "unknown cmd"}};
    }
    try {
      server.send(vox::msg::make(cmd == "snapshot" ? vox::msg::kTypeSnapshot
                                                   : vox::msg::kTypeStatus,
                                 "rk.recorder", reply_payload)
                      .dump());
    } catch (const std::exception& e) {
      // 应答失败（对端REQ先消失等）不得终止控制线程；REP 状态机由 ZMQ 侧复位
      VOX_ERROR("应答发送失败: %s", e.what());
    }
  }

  g_stop = 1;  // 退出路径统一置位（run_seconds 到期时控制/管线自然退出但检测线程阻塞在 cv）
  {
    std::lock_guard<std::mutex> lk(dslot.mu);
    dslot.cv.notify_all();
  }
  if (detect_thread.joinable()) detect_thread.join();
  pipeline.join();
  VOX_INFO("recorder service exit");
  return 0;
}
