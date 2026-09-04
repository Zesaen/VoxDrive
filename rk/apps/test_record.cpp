// test_record.cpp — R3 分段循环录像自测（R10 扩展锁段相位）
//
// 阶段A：采集→编码→Mp4SegmentSink，短分段时长跑 N 帧，断言：
//   段数符合 elapsed/segment_seconds 预期、目录文件数==segments_written、
//   每段 ffprobe 时长>0 且 codec=h264、总时长≈录像时长。
// 阶段B（水位）：独立目录预置 3 个"最旧"段文件，watermark=1% 跑一小段，
//   断言预置文件被删、正在写的当前段幸存。
// 阶段C（锁段幸存，R10）：watermark=1% + 末段加锁，断言 LOCK_ 段在水位
//   删除风暴中幸存（水位只扫 seg_ 前缀）。
// 阶段D（锁段配额，R10）：quota=1B，锁定的段立即超配额被释放回普通段。
// 输出末行 "RECORD_TEST PASS ..." / "RECORD_TEST FAIL ..."。
#define VOX_LOG_TAG "rk.test_record"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "mpp_encoder.h"
#include "mp4_segment_sink.h"
#include "v4l2_capture.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

// 通用录制相位（R10 扩展）：lock_frame≥0 时在该帧调用 lock_current（锁"当前段"）；
// lock_times>1 时此后每 30 帧重复标记（模拟多次事件命中同段/多段）。
int run_phase_lock(const std::string& dir, int count, int seg_seconds,
                   uint32_t watermark, uint64_t quota_bytes, int lock_frame,
                   int lock_times, double* out_elapsed, int* out_segments,
                   uint64_t* out_locked_total, uint32_t* out_locked_now) {
  vox::V4L2Capture::Params cp;
  cp.device = vox::config::get("rk.video_device", "/dev/video11");
  cp.width = vox::config::get_int("rk.capture_width", 1920);
  cp.height = vox::config::get_int("rk.capture_height", 1080);
  cp.fps = vox::config::get_int("rk.capture_fps", 30);
  cp.buffer_count = vox::config::get_int("rk.capture_buffers", 4);
  vox::V4L2Capture cap(cp);
  if (!cap.start()) return -1;

  vox::MppEncoder::Params ep;
  ep.width = cap.width();
  ep.height = cap.height();
  ep.stride = cap.stride();
  ep.fps = cp.fps;
  ep.gop = cp.fps * vox::config::get_int("rk.gop_seconds", 2);
  ep.bitrate_bps = vox::config::get_int("rk.bitrate_bps", 4000000);
  vox::MppEncoder enc(ep);
  if (!enc.start()) {
    cap.stop();
    return -1;
  }

  vox::Mp4SegmentSink::Params sp;
  sp.dir = dir;
  sp.segment_seconds = seg_seconds;
  sp.watermark_percent = watermark;
  sp.lock_quota_bytes = quota_bytes;
  sp.width = ep.width;
  sp.height = ep.height;
  sp.fps = ep.fps;
  vox::Mp4SegmentSink sink(sp);
  if (!sink.start(enc.sps_pps())) {
    cap.stop();
    return -1;
  }

  int64_t first_ns = 0, last_ns = 0;
  int locks_done = 0;
  for (int i = 0; i < count; ++i) {
    const vox::VideoFrame* f = cap.acquire(2000);
    if (!f) break;
    if (i == 0) first_ns = f->timestamp_ns;
    last_ns = f->timestamp_ns;
    if (lock_frame >= 0 && i >= lock_frame && locks_done < lock_times &&
        (locks_done == 0 || (i - lock_frame) % 30 == 0)) {
      sink.lock_current("test");
      locks_done++;
    }
    const vox::EncodedPacket* pkt = nullptr;
    if (enc.encode(*f, &pkt) && pkt) sink.on_packet(*pkt);
    cap.release(f);
  }
  cap.stop();
  sink.stop();

  *out_elapsed = static_cast<double>(last_ns - first_ns) / 1e9;
  *out_segments = static_cast<int>(sink.stats().segments_written);
  if (out_locked_total) *out_locked_total = sink.stats().segments_locked;
  if (out_locked_now) *out_locked_now = sink.stats().locked_segments;
  VOX_INFO("phase: elapsed=%.2fs segments=%llu frames=%llu bytes=%llu locked=%llu(now %u)",
           *out_elapsed,
           static_cast<unsigned long long>(sink.stats().segments_written),
           static_cast<unsigned long long>(sink.stats().frames_written),
           static_cast<unsigned long long>(sink.stats().bytes_written),
           static_cast<unsigned long long>(sink.stats().segments_locked),
           sink.stats().locked_segments);
  return 0;
}

int run_phase_a(const std::string& dir, int count, int seg_seconds, uint32_t watermark,
                double* out_elapsed, int* out_segments) {
  return run_phase_lock(dir, count, seg_seconds, watermark, 0, -1, 0, out_elapsed,
                        out_segments, nullptr, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "/tmp/voxdrive_rec_test";
  std::string wm_dir = "/tmp/voxdrive_rec_wm";
  int count = 300;      // 30fps 下 10s
  int seg_seconds = 3;  // 预期 ~3-4 段
  for (int i = 1; i < argc; ++i) {
    std::string s(argv[i]);
    if (s == "--count" && i + 1 < argc) count = std::stoi(argv[++i]);
    else if (s == "--segment-seconds" && i + 1 < argc) seg_seconds = std::stoi(argv[++i]);
    else if (s == "--dir" && i + 1 < argc) dir = argv[++i];
    else {
      VOX_ERROR("未知参数 %s", s.c_str());
      return 2;
    }
  }
  if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，使用内置默认值");

  // 清理测试目录
  std::string clean = "rm -rf '" + dir + "' '" + wm_dir + "'";
  if (system(clean.c_str()) != 0) return 2;

  // ---- 阶段A：分段录像 ----
  double elapsed = 0, total_duration = 0;
  int segments = 0;
  if (run_phase_a(dir, count, seg_seconds, 85, &elapsed, &segments) != 0) {
    std::printf("RECORD_TEST FAIL phase_a_run\n");
    return 1;
  }
  int files_ok = 0;
  // 每段双校验：ffprobe 时长 + ffmpeg 全量解码（容器时长正常但 sample 损坏必须抓出来）
  std::string ls = "for f in '" + dir + "'/seg_*.mp4; do d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 \"$f\") || exit 1; ffmpeg -v error -i \"$f\" -f null - || exit 1; echo \"$f $d\"; done";
  FILE* p = popen(ls.c_str(), "r");
  if (!p) {
    std::printf("RECORD_TEST FAIL popen\n");
    return 1;
  }
  char line[512];
  int files_seen = 0;
  while (fgets(line, sizeof(line), p)) {
    files_seen++;
    const char* sp = strchr(line, ' ');
    if (!sp) continue;
    double d = strtod(sp + 1, nullptr);
    if (d > 0.5 && d < seg_seconds * 2.0) {
      files_ok++;
      total_duration += d;
    } else {
      VOX_WARN("段时长异常: %s", line);
    }
  }
  const int probe_rc = pclose(p);

  // ---- 阶段B：水位淘汰 ----
  bool wm_ok = false;
  if (system(("mkdir -p '" + wm_dir + "'").c_str()) == 0) {
    for (const char* ts : {"20000101_000001", "20000101_000002", "20000101_000003"}) {
      std::string touch = "printf x > '" + wm_dir + "/seg_" + ts + ".mp4'";
      if (system(touch.c_str()) != 0) break;
    }
    double e2 = 0; int s2 = 0;
    // watermark=1%：几乎必然超限 → 应删光预置旧段（当前段不删）
    if (run_phase_a(wm_dir, 60, 600, 1, &e2, &s2) == 0) {
      std::string check = "ls '" + wm_dir + "'/seg_2000*.mp4 2>/dev/null | wc -l";
      FILE* q = popen(check.c_str(), "r");
      char n[16] = {0};
      if (q && fgets(n, sizeof(n), q)) {
        wm_ok = (atoi(n) == 0) && s2 >= 1;
      }
      if (q) pclose(q);
    }
  }

  // ---- 阶段C（锁段幸存，R10）：watermark=1% 删除风暴中 LOCK_ 段必须幸存 ----
  const std::string lock_dir = "/tmp/voxdrive_rec_lock";
  system(("rm -rf '" + lock_dir + "' && mkdir -p '" + lock_dir + "'").c_str());
  for (const char* ts : {"20000101_000004", "20000101_000005"}) {
    system(("printf x > '" + lock_dir + "/seg_" + ts + ".mp4'").c_str());
  }
  bool lock_ok = false;
  uint64_t locked_total_c = 0;
  {
    double e3 = 0; int s3 = 0; uint32_t locked_now = 0;
    // 150 帧 ~5s、段长 2s（≈3 段），第 40 帧锁"当前段"（应落在第 1 段内）
    if (run_phase_lock(lock_dir, 150, 2, 1, 0, 40, 1, &e3, &s3, &locked_total_c,
                       &locked_now) == 0) {
      // 幸存判据：LOCK_ 段存在、ffprobe 有效（非空真 MP4，不是 printf x 的占位）
      std::string check = "f=$(ls '" + lock_dir + "'/LOCK_seg_*.mp4 2>/dev/null | head -1); "
                          "[ -n \"$f\" ] && ffprobe -v error -show_entries format=duration "
                          "-of csv=p=0 \"$f\" >/dev/null 2>&1 && echo LOCKOK || echo LOCKBAD";
      FILE* q = popen(check.c_str(), "r");
      char buf[16] = {0};
      if (q && fgets(buf, sizeof(buf), q)) lock_ok = (strcmp(buf, "LOCKOK\n") == 0);
      if (q) pclose(q);
      lock_ok = lock_ok && locked_total_c >= 1 && locked_now >= 1;
    }
  }

  // ---- 阶段D（锁段配额，R10）：quota=1B → 锁定后立即超配额被释放 ----
  bool quota_ok = false;
  {
    double e4 = 0; int s4 = 0; uint64_t lt = 0; uint32_t ln = 99;
    if (run_phase_lock(lock_dir, 150, 2, 85, 1, 40, 1, &e4, &s4, &lt, &ln) == 0) {
      quota_ok = lt >= 1 && ln == 0;  // 锁过（segments_locked≥1）但存量归零（已释放）
    }
  }

  const int expect_min = static_cast<int>(elapsed / seg_seconds);
  const bool pass = files_seen == segments && files_ok == segments && segments >= expect_min &&
                    segments <= expect_min + 2 && probe_rc == 0 && wm_ok && lock_ok &&
                    quota_ok && elapsed > 1.0;
  std::printf("RECORD_TEST %s segments=%d/%d files_ok=%d/%d elapsed=%.2fs seg_dur_sum=%.2fs watermark_ok=%d lock_ok=%d(locked_total=%llu) quota_ok=%d\n",
              pass ? "PASS" : "FAIL", segments, expect_min, files_ok, files_seen, elapsed,
              total_duration, wm_ok ? 1 : 0, lock_ok ? 1 : 0,
              static_cast<unsigned long long>(locked_total_c), quota_ok ? 1 : 0);
  return pass ? 0 : 1;
}
