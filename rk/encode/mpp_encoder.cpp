// mpp_encoder.cpp — MPP H.264 硬编码实现
#define VOX_LOG_TAG "rk.encoder"

#include "mpp_encoder.h"

#include <string.h>

#include <chrono>
#include <thread>

#include "vox_log.h"

#include "rk_venc_cfg.h"

namespace vox {
namespace {
constexpr uint32_t mpp_align(uint32_t x, uint32_t a) {
  return (x + a - 1) & ~(a - 1);
}
}  // namespace

MppEncoder::MppEncoder(const Params& p) : params_(p) {
  if (params_.stride == 0) params_.stride = params_.width;
}

MppEncoder::~MppEncoder() { stop(); }

bool MppEncoder::start() {
  if (initialized_) return true;

  MPP_RET ret = mpp_create(&mpp_ctx_, &mpp_mpi_);
  if (ret != MPP_OK) {
    VOX_ERROR("mpp_create ret=%d", ret);
    return false;
  }
  ret = mpp_init(mpp_ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
  if (ret != MPP_OK) {
    VOX_ERROR("mpp_init(ENC/AVC) ret=%d", ret);
    stop();
    return false;
  }
  // MPP 要求 stride 对齐（NV12：hor 按 16 字节，ver 按 16 行）
  hor_stride_ = mpp_align(params_.stride, 16);
  ver_stride_ = mpp_align(params_.height, 16);

  if (!apply_config()) {
    stop();
    return false;
  }
  ret = mpp_buffer_group_get_internal(&frm_grp_, MPP_BUFFER_TYPE_ION);
  if (ret != MPP_OK) {
    VOX_ERROR("buffer group ret=%d", ret);
    stop();
    return false;
  }
  if (!fetch_sps_pps()) {
    stop();
    return false;
  }
  initialized_ = true;
  VOX_INFO("started: %s", describe().c_str());
  VOX_INFO("sps/pps %zu bytes", sps_pps_.size());
  return true;
}

bool MppEncoder::start(const Params& p) {
  if (initialized_) return params_.width == p.width && params_.height == p.height;
  params_ = p;
  if (params_.stride == 0) params_.stride = params_.width;
  return start();
}

void MppEncoder::stop() {
  if (frm_grp_) {
    mpp_buffer_group_put(frm_grp_);
    frm_grp_ = nullptr;
  }
  if (enc_cfg_) {
    mpp_enc_cfg_deinit(enc_cfg_);
    enc_cfg_ = nullptr;
  }
  if (mpp_ctx_) {
    mpp_destroy(mpp_ctx_);
    mpp_ctx_ = nullptr;
  }
  initialized_ = false;
}

bool MppEncoder::apply_config() {
  MPP_RET ret = mpp_enc_cfg_init(&enc_cfg_);
  if (ret != MPP_OK) {
    VOX_ERROR("mpp_enc_cfg_init ret=%d", ret);
    return false;
  }
  ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_CFG, enc_cfg_);
  if (ret != MPP_OK) {
    VOX_ERROR("GET_CFG ret=%d", ret);
    return false;
  }

  // 输入图像：NV12 半平面（rkisp 直出）
  mpp_enc_cfg_set_s32(enc_cfg_, "prep:width", params_.width);
  mpp_enc_cfg_set_s32(enc_cfg_, "prep:height", params_.height);
  mpp_enc_cfg_set_s32(enc_cfg_, "prep:hor_stride", hor_stride_);
  mpp_enc_cfg_set_s32(enc_cfg_, "prep:ver_stride", ver_stride_);
  mpp_enc_cfg_set_s32(enc_cfg_, "prep:format", MPP_FMT_YUV420SP);

  // 码率控制：VBR，目标 bps、上下限 ±50%
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:mode", MPP_ENC_RC_MODE_VBR);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_target", params_.bitrate_bps);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_max", params_.bitrate_bps * 3 / 2);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_min", params_.bitrate_bps / 2);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_flex", 0);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_num", params_.fps);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_denom", 1);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_flex", 0);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_num", params_.fps);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_denom", 1);
  mpp_enc_cfg_set_s32(enc_cfg_, "rc:gop", params_.gop);

  // H.264：Baseline（无 B 帧、低延迟，行车记录场景），QP 区间沿用 prj2 实证参数
  mpp_enc_cfg_set_s32(enc_cfg_, "codec:type", MPP_VIDEO_CodingAVC);
  mpp_enc_cfg_set_s32(enc_cfg_, "h264:profile", 66);
  mpp_enc_cfg_set_s32(enc_cfg_, "h264:level", 40);
  mpp_enc_cfg_set_s32(enc_cfg_, "h264:cabac_en", 0);
  mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_init", 26);
  mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_min", 20);
  mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_max", 35);

  ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_SET_CFG, enc_cfg_);
  if (ret != MPP_OK) {
    VOX_ERROR("SET_CFG ret=%d", ret);
    return false;
  }
  return true;
}

bool MppEncoder::fetch_sps_pps() {
  MppPacket packet = nullptr;
  MPP_RET ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_EXTRA_INFO, &packet);
  if (ret != MPP_OK || !packet) {
    VOX_ERROR("GET_EXTRA_INFO ret=%d", ret);
    return false;
  }
  const void* data = mpp_packet_get_data(packet);
  const size_t len = mpp_packet_get_length(packet);
  if (!data || len == 0) {
    VOX_ERROR("extra info 为空");
    return false;
  }
  sps_pps_.assign(static_cast<const uint8_t*>(data),
                  static_cast<const uint8_t*>(data) + len);
  // packet 归 MPP 上下文所有，不 deinit（prj2 此处写法有二次释放隐患）
  return true;
}

void MppEncoder::copy_nv12_in(const VideoFrame& in, uint8_t* dst) {
  const uint32_t w = params_.width;
  // 逐平面行距：Y/UV 行距可能不同（rkisp 单平面 NV12 混合布局，见 v4l2_capture）
  const auto copy_plane = [&](const uint8_t* src, size_t src_stride, uint32_t rows,
                              uint32_t row_bytes) {
    if (src_stride == hor_stride_ && row_bytes == hor_stride_) {
      // 快路径：源与目标 stride 一致，整块拷贝
      memcpy(dst, src, static_cast<size_t>(hor_stride_) * rows);
    } else {
      for (uint32_t r = 0; r < rows; ++r) {
        memcpy(dst + static_cast<size_t>(r) * hor_stride_,
               src + static_cast<size_t>(r) * src_stride, row_bytes);
      }
    }
  };
  // Y 平面（ver_stride 对齐产生的 padding 行清零，避免读到未初始化内存）
  copy_plane(static_cast<const uint8_t*>(in.plane[0]), in.plane_stride[0],
             params_.height, w);
  memset(dst + static_cast<size_t>(hor_stride_) * params_.height, 0,
         static_cast<size_t>(hor_stride_) * (ver_stride_ - params_.height));
  // UV 平面位于 hor*ver 偏移处（半平面交错，行数为高一半）
  uint8_t* dst_uv = dst + static_cast<size_t>(hor_stride_) * ver_stride_;
  copy_plane(static_cast<const uint8_t*>(in.plane[1]), in.plane_stride[1],
             params_.height / 2, w);
  memset(dst_uv + static_cast<size_t>(hor_stride_) * (params_.height / 2), 0,
         static_cast<size_t>(hor_stride_) * (ver_stride_ / 2 - params_.height / 2));
}

bool MppEncoder::encode(const VideoFrame& in, const EncodedPacket** out) {
  *out = nullptr;
  if (!initialized_) {
    VOX_ERROR("编码器未初始化");
    return false;
  }

  MppFrame frame = nullptr;
  MppBuffer buffer = nullptr;
  MppPacket packet = nullptr;

  MPP_RET ret = mpp_frame_init(&frame);
  if (ret != MPP_OK) {
    VOX_ERROR("mpp_frame_init ret=%d", ret);
    return false;
  }
  const size_t buf_size = static_cast<size_t>(hor_stride_) * ver_stride_ * 3 / 2;
  ret = mpp_buffer_get(frm_grp_, &buffer, buf_size);
  if (ret != MPP_OK) {
    VOX_ERROR("mpp_buffer_get ret=%d", ret);
    mpp_frame_deinit(&frame);
    return false;
  }
  copy_nv12_in(in, static_cast<uint8_t*>(mpp_buffer_get_ptr(buffer)));
  // ION 缓冲是 cached 映射：CPU 写入后必须 flush（clean）才能让编码器硬件看到。
  // 缺这步时最后写入的 UV 平面常驻 CPU 缓存、硬件读到清零内存→码流无色度（纯绿画面）
  mpp_buffer_sync_end(buffer);

  mpp_frame_set_width(frame, params_.width);
  mpp_frame_set_height(frame, params_.height);
  mpp_frame_set_hor_stride(frame, hor_stride_);
  mpp_frame_set_ver_stride(frame, ver_stride_);
  mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
  mpp_frame_set_pts(frame, in.timestamp_ns / 1000);  // MPP pts 单位 us
  mpp_frame_set_buffer(frame, buffer);
  mpp_frame_set_eos(frame, 0);

  ret = mpp_mpi_->encode_put_frame(mpp_ctx_, frame);
  if (ret != MPP_OK) {
    VOX_ERROR("encode_put_frame ret=%d", ret);
    mpp_buffer_put(buffer);
    mpp_frame_deinit(&frame);
    return false;
  }

  // 轮询取包（编码器可能晚一拍，1ms×50 上限，沿用 prj2 实证节奏）
  for (int r = 0; r < 50; ++r) {
    ret = mpp_mpi_->encode_get_packet(mpp_ctx_, &packet);
    if (ret == MPP_OK && packet) break;
    if (ret != MPP_ERR_TIMEOUT) break;  // 无包（跳帧等）按正常处理
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  mpp_frame_deinit(&frame);
  mpp_buffer_put(buffer);  // 与 prj2 一致：取到 packet 后再归还输入缓冲
  if (ret != MPP_OK || !packet) return true;  // 帧已消费，本轮无输出（*out 保持 null）

  const uint8_t* data = static_cast<const uint8_t*>(mpp_packet_get_data(packet));
  const size_t len = mpp_packet_get_length(packet);
  if (!data || len == 0) {
    mpp_packet_deinit(&packet);
    return true;
  }
  pkt_buf_.assign(data, data + len);
  mpp_packet_deinit(&packet);

  // 扫描包内全部 NAL 判关键帧（起始码 00 00 01——兼容 3/4 字节；
  // MPP 的 IDR 包常带前缀 SEI，只看首个 NAL 会漏判）
  bool keyframe = false;
  for (size_t i = 0; i + 3 < len; ++i) {
    if (pkt_buf_[i] == 0 && pkt_buf_[i + 1] == 0 && pkt_buf_[i + 2] == 1) {
      if ((pkt_buf_[i + 3] & 0x1f) == 5) {
        keyframe = true;
        break;
      }
    }
  }
  pkt_ = EncodedPacket{};
  pkt_.data = pkt_buf_.data();
  pkt_.size = pkt_buf_.size();
  pkt_.is_keyframe = keyframe;
  pkt_.timestamp_ns = in.timestamp_ns;
  pkt_.sequence = in.sequence;
  *out = &pkt_;
  return true;
}

std::string MppEncoder::describe() const {
  char s[192];
  std::snprintf(s, sizeof(s),
                "H.264 baseline %ux%u NV12 stride=%u/%u, VBR %u kbps, gop=%u, %ufps",
                params_.width, params_.height, params_.stride, hor_stride_,
                params_.bitrate_bps / 1000, params_.gop, params_.fps);
  return s;
}

}  // namespace vox
