// mpp_encoder.h — Rockchip MPP H.264 硬编码器（R2；底本=prj2 MppEncoder，改采集输入链）
//
// 与 prj2 原版（解码后 BGR 帧）的差异：
//   - 输入 NV12 半平面（rkisp 直出格式），主码流同尺寸同格式直入 MPP，
//     不再需要 RGA 做 BGR→YUV 转换；RGA 留给 R11 子码流缩放
//   - 输入帧采集时间戳写入 frame pts（C3 MP4 封装、R9 延迟对账用）
//   - SPS/PPS 拷贝返回，不 deinit MPP 内部 packet（prj2 写法有二次释放隐患）
//   - 输出 EncodedPacket（Annex-B 含 00 00 00 01 起始码），供 IVideoSink 扇出
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rk_mpi.h"
#include "vox/video_sink.h"
#include "vox/video_source.h"

namespace vox {

class MppEncoder {
 public:
  struct Params {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t stride = 0;             // NV12 行跨度（字节），0=width
    uint32_t fps = 30;
    uint32_t gop = 60;               // GOP 帧数（行车记录 1-2s 一个 I 帧）
    uint32_t bitrate_bps = 4000000;  // VBR 目标码率（bps）
  };

  explicit MppEncoder(const Params& p);
  ~MppEncoder();

  MppEncoder(const MppEncoder&) = delete;
  MppEncoder& operator=(const MppEncoder&) = delete;

  bool start();
  void stop();
  bool is_started() const { return initialized_; }

  // 编码一帧 NV12。返回 false=硬错误；true 时 *out 可能非空（编码输出，仅在下一次
  // encode()/stop() 前有效）或为空（帧已消费但本轮无输出，如编码器跳帧）。
  bool encode(const VideoFrame& in, const EncodedPacket** out);

  // SPS/PPS extradata（Annex-B，含起始码），start() 成功后有效
  const std::vector<uint8_t>& sps_pps() const { return sps_pps_; }

  std::string describe() const;

 private:
  bool apply_config();
  bool fetch_sps_pps();
  void copy_nv12_in(const VideoFrame& in, uint8_t* dst);  // V4L2 平面 → MPP 帧缓冲（含对齐 padding）

  Params params_;
  MppCtx mpp_ctx_ = nullptr;
  MppApi* mpp_mpi_ = nullptr;
  MppEncCfg enc_cfg_ = nullptr;
  MppBufferGroup frm_grp_ = nullptr;
  uint32_t hor_stride_ = 0;  // MPP 对齐后行跨度
  uint32_t ver_stride_ = 0;  // MPP 对齐后垂直跨度
  bool initialized_ = false;

  std::vector<uint8_t> sps_pps_;
  std::vector<uint8_t> pkt_buf_;  // 编码输出缓冲（复用，避免每帧分配）
  EncodedPacket pkt_{};
};

}  // namespace vox
