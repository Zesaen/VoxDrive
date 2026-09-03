// v4l2_capture.h — IVideoSource 的 V4L2 实现（R1）
//
// 链路：IMX415(RAW) → MIPI CSI-2 → rkcif → rkisp(mainpath) → NV12 mmap 出帧。
// 传感器/ISP 侧是厂商 BSP 提供的驱动与管线（配置级），本类只做用户态取帧：
// 格式协商（S_FMT 后以 G_FMT 实际值为准）、REQBUFS/mmap 多缓冲、
// EXPBUF 导出 DMA-BUF fd（供 MPP/RGA 零拷贝）、poll+DQBUF 出队。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "vox/video_source.h"

struct v4l2_buffer;

namespace vox {

class V4L2Capture final : public IVideoSource {
 public:
  struct Params {
    std::string device = "/dev/video11";  // rkisp mainpath
    uint32_t width = 1920;
    uint32_t height = 1080;
    PixelFormat format = PixelFormat::NV12;
    uint32_t buffer_count = 4;  // mmap 缓冲数（>=3 保证采集与消费解耦）
    uint32_t fps = 30;          // 期望帧率（S_PARM 请求，实际以协商为准）
  };

  explicit V4L2Capture(const Params& params);
  ~V4L2Capture() override;

  V4L2Capture(const V4L2Capture&) = delete;
  V4L2Capture& operator=(const V4L2Capture&) = delete;

  bool start() override;
  void stop() override;
  bool is_running() const override { return running_.load(); }
  const VideoFrame* acquire(int timeout_ms) override;
  void release(const VideoFrame* frame) override;
  std::string describe() const override;

  // 协商后的实际参数（start() 前为构造值；驱动可能调整尺寸/stride）
  uint32_t width() const { return params_.width; }
  uint32_t height() const { return params_.height; }
  uint32_t stride() const { return static_cast<uint32_t>(cached_stride_); }

 private:
  struct MappedBuffer {
    void* start = nullptr;
    size_t length = 0;
    int dma_fd = -1;
  };

  bool open_device();
  void close_device();
  bool negotiate_format();
  bool request_and_map_buffers();
  void unmap_buffers();
  bool start_streaming();
  bool enqueue(int index);
  bool dequeue_oldest();  // DQBUF 填充 current_frame_，记 dequeued_index_

  static uint32_t to_v4l2_fourcc(PixelFormat fmt);

  Params params_;
  int fd_ = -1;
  uint32_t v4l2_buf_type_ = 0;      // V4L2_BUF_TYPE_VIDEO_CAPTURE(_MPLANE)
  uint32_t negotiated_planes_ = 0;  // 驱动报告的平面数（rkisp NV12 为 1，Y/UV 连续）
  size_t cached_stride_ = 0;        // 协商出的行跨度（negotiate 时缓存，出帧不查 G_FMT）
  std::vector<MappedBuffer> buffers_;
  int dequeued_index_ = -1;  // 当前在调用方手中的缓冲（-1=无）
  VideoFrame current_frame_;
  uint64_t frame_counter_ = 0;
  std::atomic<bool> running_{false};
};

// 枚举 /dev/video* 采集设备（card/driver/当前格式），返回人读多行文本。
// 排障用（R1 的"设备枚举"项），不依赖实例。
std::string list_video_devices();

}  // namespace vox
