// test_detect.cpp — 事件锁录检测链独立自测（R10）
//
// 验证：NV12 采集帧 → RGA → RKNN(NPU core0) → 类别过滤/NMS 的完整链路；
// 报告推理耗时（ewma/max）、检测命中、以及检测运行期间采集 fps（应保持 30）。
// 用法：test_detect [--seconds 10] [--all-classes] [--interval 30]
//   --all-classes  不过滤类别（室内验证链路用——画面里有什么测什么）
// PASS 判据：推理≥1 次、无错误、采集 fps 与无检测基线一致（±5%）。
#define VOX_LOG_TAG "rk.test"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>

#include "json.hpp"
#include "rknn_detector.h"
#include "v4l2_capture.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

int64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// NV12 带行距 → 紧凑拷贝（与 recorder_service 同逻辑）
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

}  // namespace

int main(int argc, char** argv) {
  double seconds = 10;
  int interval = 30;
  bool all_classes = false;
  for (int i = 1; i < argc; ++i) {
    std::string s(argv[i]);
    if (s == "--seconds" && i + 1 < argc) seconds = std::stod(argv[++i]);
    else if (s == "--interval" && i + 1 < argc) interval = std::stoi(argv[++i]);
    else if (s == "--all-classes") all_classes = true;
  }
  vox::config::load();

  vox::V4L2Capture::Params cp;
  cp.device = vox::config::get("rk.video_device", "/dev/video11");
  cp.width = vox::config::get_int("rk.capture_width", 1920);
  cp.height = vox::config::get_int("rk.capture_height", 1080);
  cp.fps = vox::config::get_int("rk.capture_fps", 30);
  cp.buffer_count = vox::config::get_int("rk.capture_buffers", 4);

  vox::RknnDetector::Params dp;
  dp.model_path =
      vox::config::get("rk.detect_model", "$HOME/Desktop/VoxDrive/models/yolov5s-640-640.rknn");
  if (!all_classes) {
    std::string cs = vox::config::get("rk.detect_classes", "0,2,3,5,7");
    for (char* tok = std::strtok(cs.data(), ", "); tok; tok = std::strtok(nullptr, ", "))
      dp.target_classes.insert(std::atoi(tok));
  }

  vox::RknnDetector det;
  if (!det.start(dp)) {
    std::printf("DETECT_TEST FAIL init: %s\n", det.error().c_str());
    return 1;
  }
  vox::V4L2Capture cap(cp);
  if (!cap.start()) {
    std::printf("DETECT_TEST FAIL capture\n");
    return 1;
  }
  std::printf("[detect-test] model=%s (%dx%d C=%d) classes=%s interval=%d帧\n",
              dp.model_path.c_str(), det.model_width(), det.model_height(),
              det.class_num(), all_classes ? "ALL" : "person/car/motor/bus/truck",
              interval);

  // 基线：无检测 2s 的采集 fps
  int64_t t0 = steady_ns();
  int base_frames = 0;
  int64_t base_last = 0;
  while (steady_ns() - t0 < 2000000000ll) {
    const vox::VideoFrame* f = cap.acquire(2000);
    if (!f) continue;
    base_last = f->timestamp_ns;
    cap.release(f);
    base_frames++;
  }

  // 检测运行：主循环同步推理（独立自测不建线程；recorder_service 里是异步线程）
  t0 = steady_ns();
  int frames = 0, infers = 0, hits = 0;
  uint64_t last_seq = 0;
  double fps = 0.0;
  while (steady_ns() - t0 < static_cast<int64_t>(seconds * 1e9)) {
    const vox::VideoFrame* f = cap.acquire(2000);
    if (!f) continue;
    ++frames;
    if (f->sequence % interval == 0 && f->sequence != last_seq) {
      last_seq = f->sequence;
      std::vector<uint8_t> nv12;
      pack_nv12(f, nv12);
      auto dets = det.detect_nv12(nv12.data(), static_cast<int>(f->width),
                                  static_cast<int>(f->height));
      infers++;
      if (!dets.empty()) {
        hits += static_cast<int>(dets.size());
        for (const auto& d : dets)
          std::printf("  [hit] cls=%d conf=%.2f box=(%d,%d)-(%d,%d)\n", d.cls, d.prop,
                      d.left, d.top, d.right, d.bottom);
      }
    }
    cap.release(f);
  }
  const double dt = (steady_ns() - t0) / 1e9;
  fps = frames / dt;
  const double base_fps = base_frames / 2.0;
  const auto& s = det.stats();
  (void)base_last;

  std::printf("[detect-test] 采集 %d 帧 %.1ffps（基线 %.1ffps）| 推理 %d 次，命中 %d 目标 | "
              "耗时 ewma %.1fms max %.1fms\n",
              frames, fps, base_fps, infers, hits, s.infer_ms_ewma, s.infer_ms_max);

  cap.stop();
  // 判据：真实推理发生（耗时非零）且采集 fps 不掉（检测不拖累主管线）
  const bool pass = infers >= 1 && s.infer_ms_ewma > 0.0 && fps >= base_fps * 0.95;
  std::printf("DETECT_TEST %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
