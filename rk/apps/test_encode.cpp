// test_encode.cpp — R2 编码链自测（采集 → MPP H.264 → 裸流落盘 + 统计）
//
// 用法：test_encode [--count N] [--out /tmp/test_encode.h264]
// 统计口径：帧率用驱动时间戳；码率=输出总字节*8/时长；编码延迟=encode() 调用耗时。
// 输出末行 "ENCODE_TEST PASS ..." / "ENCODE_TEST FAIL ..."。
#define VOX_LOG_TAG "rk.test_encode"

#include <stdio.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "mpp_encoder.h"
#include "v4l2_capture.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

int main(int argc, char** argv) {
  int count = 300;  // 30fps 下 10s
  const char* out_path = "/tmp/test_encode.h264";
  bool synth = false;  // --synth：不用采集，喂强色度合成帧（隔离 V4L2 排障）
  for (int i = 1; i < argc; ++i) {
    std::string s(argv[i]);
    if (s == "--count" && i + 1 < argc) count = std::stoi(argv[++i]);
    else if (s == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (s == "--synth") synth = true;
    else {
      VOX_ERROR("未知参数 %s", s.c_str());
      return 2;
    }
  }

  if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，使用内置默认值");

  // ---- 采集 ----
  vox::V4L2Capture::Params cp;
  cp.device = vox::config::get("rk.video_device", "/dev/video11");
  cp.width = vox::config::get_int("rk.capture_width", 1920);
  cp.height = vox::config::get_int("rk.capture_height", 1080);
  cp.fps = vox::config::get_int("rk.capture_fps", 30);
  cp.buffer_count = vox::config::get_int("rk.capture_buffers", 4);
  vox::V4L2Capture cap(cp);
  if (!synth && !cap.start()) {
    std::printf("ENCODE_TEST FAIL capture_start\n");
    return 1;
  }

  // ---- 编码（与采集协商结果对齐）----
  vox::MppEncoder::Params ep;
  ep.width = synth ? cp.width : cap.width();
  ep.height = synth ? cp.height : cap.height();
  ep.stride = synth ? cp.width : cap.stride();
  ep.fps = cp.fps;
  ep.gop = cp.fps * vox::config::get_int("rk.gop_seconds", 2);
  ep.bitrate_bps = vox::config::get_int("rk.bitrate_bps", 4000000);
  vox::MppEncoder enc(ep);
  if (!enc.start()) {
    if (!synth) cap.stop();
    std::printf("ENCODE_TEST FAIL encoder_start\n");
    return 1;
  }

  // 合成帧缓冲：强色度（U=100 偏绿 / V=200 偏红，肉眼应见橙红），Y 横向渐变
  std::vector<uint8_t> synth_y, synth_uv;
  if (synth) {
    synth_y.resize(static_cast<size_t>(ep.width) * ep.height);
    synth_uv.resize(static_cast<size_t>(ep.width) * ep.height / 2);
    for (uint32_t r = 0; r < ep.height; ++r) {
      std::memset(synth_y.data() + static_cast<size_t>(r) * ep.width,
                  40 + (r * 160u) / ep.height, ep.width);  // 纵向 Y 渐变
    }
    std::memset(synth_uv.data(), 0, synth_uv.size());
    for (size_t i = 0; i < synth_uv.size() / 2; ++i) {
      synth_uv[2 * i] = 100;   // U 低 → 偏绿成分
      synth_uv[2 * i + 1] = 200;  // V 高 → 偏红
    }
  }

  FILE* out = fopen(out_path, "wb");
  if (!out) {
    VOX_ERROR("打开输出文件 %s 失败", out_path);
    cap.stop();
    return 1;
  }
  fwrite(enc.sps_pps().data(), 1, enc.sps_pps().size(), out);  // 裸流头部 SPS/PPS

  uint64_t encoded = 0, keyframes = 0, total_bytes = 0;
  int64_t first_ns = 0, last_ns = 0;
  double enc_ms_total = 0, enc_ms_max = 0;

  for (int i = 0; i < count; ++i) {
    vox::VideoFrame synth_frame{};
    const vox::VideoFrame* f;
    if (synth) {
      synth_frame.width = ep.width;
      synth_frame.height = ep.height;
      synth_frame.format = vox::PixelFormat::NV12;
      synth_frame.timestamp_ns = now_ns();
      synth_frame.sequence = i;
      synth_frame.plane[0] = synth_y.data();
      synth_frame.plane_stride[0] = ep.width;
      synth_frame.plane[1] = synth_uv.data();
      synth_frame.plane_stride[1] = ep.width;
      f = &synth_frame;
    } else {
      f = cap.acquire(2000);
    }
    if (!f) {
      VOX_ERROR("第 %d/%d 帧获取失败", i + 1, count);
      break;
    }
    if (i == 0) first_ns = f->timestamp_ns;
    last_ns = f->timestamp_ns;

    const int64_t t0 = now_ns();
    const vox::EncodedPacket* pkt = nullptr;
    const bool ok = enc.encode(*f, &pkt);
    const double ms = (now_ns() - t0) / 1e6;
    enc_ms_total += ms;
    if (ms > enc_ms_max) enc_ms_max = ms;
    if (!synth) cap.release(f);

    if (!ok) {
      VOX_ERROR("第 %d 帧编码硬错误", i + 1);
      break;
    }
    if (pkt) {
      fwrite(pkt->data, 1, pkt->size, out);
      total_bytes += pkt->size;
      encoded++;
      if (pkt->is_keyframe) keyframes++;
      if (encoded <= 2) {
        VOX_INFO("packet #%llu size=%zu keyframe=%d", static_cast<unsigned long long>(encoded - 1),
                 pkt->size, pkt->is_keyframe ? 1 : 0);
      }
    }
  }
  cap.stop();
  enc.stop();
  fclose(out);

  const double elapsed_s =
      encoded >= 2 ? static_cast<double>(last_ns - first_ns) / 1e9 : 0.0;
  const double fps = encoded >= 2 ? (encoded - 1) / elapsed_s : 0.0;
  const double mbps = elapsed_s > 0 ? total_bytes * 8.0 / elapsed_s / 1e6 : 0.0;

  VOX_INFO("统计：captured=%d encoded=%llu keyframes=%llu bytes=%llu(%s) elapsed=%.3fs fps=%.1f 码率=%.2fMbps 编码延迟 avg=%.2fms max=%.2fms",
           count, static_cast<unsigned long long>(encoded),
           static_cast<unsigned long long>(keyframes),
           static_cast<unsigned long long>(total_bytes), out_path, elapsed_s, fps, mbps,
           encoded ? enc_ms_total / encoded : 0.0, enc_ms_max);

  // PASS 判据：帧数齐、按 GOP 节奏出 I 帧、码率在目标量级的合理带宽内
  //（静态室内场景 VBR 会大幅下探，属编码器正常行为，不按目标码率下限卡死）
  const uint64_t expect_kf = count / ep.gop;
  const bool pass = encoded == static_cast<uint64_t>(count) &&
                    keyframes + 1 >= expect_kf && mbps > 0.05 && mbps < 20.0;
  std::printf("ENCODE_TEST %s encoded=%llu/%d fps=%.1f mbps=%.2f keyframes=%llu enc_avg_ms=%.2f enc_max_ms=%.2f out=%s\n",
              pass ? "PASS" : "FAIL", static_cast<unsigned long long>(encoded), count, fps,
              mbps, static_cast<unsigned long long>(keyframes),
              encoded ? enc_ms_total / encoded : 0.0, enc_ms_max, out_path);
  return pass ? 0 : 1;
}
