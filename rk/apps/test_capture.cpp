// test_capture.cpp — R1 采集链自测（板上运行）
//
// 用法：test_capture [--list] [--count N] [--device D] [--width W] [--height H] [--fps F]
//   缺省从 voxdrive.conf 读 rk.video_device / rk.capture_width 等键。
// 输出末行 "CAPTURE_TEST PASS ..." 或 "CAPTURE_TEST FAIL ..."，供脚本判定。
#define VOX_LOG_TAG "rk.test_capture"

#include <cstring>
#include <string>

#include "v4l2_capture.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

struct Args {
  bool list = false;
  int count = 120;  // 30fps 下约 4s
  std::string device;
  int width = 0, height = 0, fps = 0;
};

Args parse_args(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string s(argv[i]);
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        VOX_ERROR("%s 缺参数", name);
        exit(2);
      }
      return argv[++i];
    };
    if (s == "--list") a.list = true;
    else if (s == "--count") a.count = std::stoi(next("--count"));
    else if (s == "--device") a.device = next("--device");
    else if (s == "--width") a.width = std::stoi(next("--width"));
    else if (s == "--height") a.height = std::stoi(next("--height"));
    else if (s == "--fps") a.fps = std::stoi(next("--fps"));
    else {
      VOX_ERROR("未知参数 %s", s.c_str());
      exit(2);
    }
  }
  return a;
}

// 首帧 Y 平面非零校验：全零说明管线空转（时钟有了、数据没到）
bool plane_has_data(const vox::VideoFrame& f) {
  const uint8_t* p = static_cast<const uint8_t*>(f.plane[0]);
  for (size_t i = 0; i < f.plane_bytes[0]; i += 4096) {
    size_t n = f.plane_bytes[0] - i < 4096 ? f.plane_bytes[0] - i : 4096;
    for (size_t j = 0; j < n; ++j) {
      if (p[i + j] != 0) return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Args args = parse_args(argc, argv);

  if (!vox::config::load()) {
    VOX_WARN("未找到 voxdrive.conf，使用内置默认值");
  }

  if (args.list) {
    VOX_INFO("采集设备枚举：\n%s", vox::list_video_devices().c_str());
    return 0;
  }

  vox::V4L2Capture::Params p;
  p.device = !args.device.empty()
                 ? args.device
                 : vox::config::get("rk.video_device", "/dev/video11");
  p.width = args.width > 0 ? static_cast<uint32_t>(args.width)
                           : vox::config::get_int("rk.capture_width", 1920);
  p.height = args.height > 0 ? static_cast<uint32_t>(args.height)
                             : vox::config::get_int("rk.capture_height", 1080);
  p.fps = args.fps > 0 ? static_cast<uint32_t>(args.fps)
                       : vox::config::get_int("rk.capture_fps", 30);
  p.buffer_count = vox::config::get_int("rk.capture_buffers", 4);

  vox::V4L2Capture cap(p);
  if (!cap.start()) {
    std::printf("CAPTURE_TEST FAIL start\n");
    return 1;
  }

  int64_t first_ns = 0, last_ns = 0;
  int64_t max_gap_ms = 0;
  uint64_t frames = 0;
  bool has_pixel_data = false;

  for (int i = 0; i < args.count; ++i) {
    const vox::VideoFrame* f = cap.acquire(2000);
    if (!f) {
      VOX_ERROR("第 %d/%d 帧获取失败", i + 1, args.count);
      break;
    }
    if (i == 0) {
      first_ns = f->timestamp_ns;
      has_pixel_data = plane_has_data(*f);
    } else {
      int64_t gap = (f->timestamp_ns - last_ns) / 1000000;
      if (gap > max_gap_ms) max_gap_ms = gap;
    }
    last_ns = f->timestamp_ns;
    frames++;
    if (i < 3 || i == args.count - 1) {
      VOX_INFO("frame seq=%llu %ux%u stride=%zu/%zu ts=%lldms dma_fd=%d",
               static_cast<unsigned long long>(f->sequence), f->width, f->height,
               f->plane_stride[0], f->plane_stride[1],
               static_cast<long long>(f->timestamp_ns / 1000000), f->dma_fd);
    }
    cap.release(f);
  }
  cap.stop();

  const double elapsed_s =
      frames >= 2 ? static_cast<double>(last_ns - first_ns) / 1e9 : 0.0;
  const double fps = frames >= 2 ? (frames - 1) / elapsed_s : 0.0;

  VOX_INFO("统计：frames=%llu elapsed=%.3fs fps=%.1f max_gap=%lldms 首帧有数据=%d",
           static_cast<unsigned long long>(frames), elapsed_s, fps,
           static_cast<long long>(max_gap_ms), has_pixel_data ? 1 : 0);

  const bool pass = frames == static_cast<uint64_t>(args.count) && fps >= 15.0 &&
                    has_pixel_data;
  std::printf("CAPTURE_TEST %s frames=%llu fps=%.1f max_gap_ms=%lld res=%ux%u\n",
              pass ? "PASS" : "FAIL", static_cast<unsigned long long>(frames), fps,
              static_cast<long long>(max_gap_ms), p.width, p.height);
  return pass ? 0 : 1;
}
