// video_source.h — 视频采集源接口（RK 节点采集链抽象层）
//
// 分层动机：采集 / 编码 / 输出三层解耦，任何一层换实现都不动主管线。
//   IVideoSource 的已知实现与预留扩展位：
//     - V4L2Capture（capture/）：IMX415 经 rkisp 出 NV12，首个实现
//     - FileSource（预留）：读_YUV 文件回放，无摄像头时跑管线联调/回归
//     - NetworkSource（预留）：拉远端流作为源
// 帧生命周期契约：acquire() 返回内部缓冲的只读视图，release() 归还后即失效；
//   同一时刻至多一帧在调用方手中（采集线程单消费者模型，不做引用计数）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vox {

// 采集帧像素格式。只登记实际用到的，避免维护空壳枚举。
enum class PixelFormat : uint8_t {
  NV12,  // 4:2:0 半平面：plane[0]=Y，plane[1]=UV 交错
};

// 一帧视频（只读视图，不拥有内存）。NV12 双平面布局。
struct VideoFrame {
  void* plane[2] = {nullptr, nullptr};   // plane[0]=Y plane[1]=UV
  size_t plane_bytes[2] = {0, 0};        // 各平面有效字节数
  size_t plane_stride[2] = {0, 0};       // 行跨度（字节）
  int dma_fd = -1;                       // DMA-BUF fd（已导出时非负；MPP/RGA 零拷贝入口）
  uint32_t width = 0;
  uint32_t height = 0;
  PixelFormat format = PixelFormat::NV12;
  int64_t timestamp_ns = 0;              // 帧采集时间戳（CLOCK_MONOTONIC，驱动填充）
  uint64_t sequence = 0;                 // 帧序号，自 start() 起递增
};

class IVideoSource {
 public:
  virtual ~IVideoSource() = default;

  // 打开设备、格式协商、分配缓冲并开流；失败返回 false（错误经日志输出）
  virtual bool start() = 0;
  // 停流并释放全部资源；可重复调用（未启动时为空操作）
  virtual void stop() = 0;
  virtual bool is_running() const = 0;

  // 阻塞至多 timeout_ms 取下一帧；超时/已停止/错误返回 nullptr
  virtual const VideoFrame* acquire(int timeout_ms) = 0;
  // 归还当前帧缓冲；下一次 acquire() 前必须调用
  virtual void release(const VideoFrame* frame) = 0;

  // 人读描述（设备/协商结果/缓冲数），用于启动日志
  virtual std::string describe() const = 0;
};

}  // namespace vox
