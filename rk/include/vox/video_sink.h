// video_sink.h — 编码后输出接口（一帧多路扇出）
//
// MPP 编码器输出的每个 H.264 访问单元同时分发到多个 sink：
//   - Mp4SegmentSink（storage/，C3）：分段循环存储
//   - RtmpSink（stream/，C4）：推流到 Jetson mediamtx
//   - RtspSink 等新传输方式（预留）：只加实现，不动主管线
// 跨板 ZMQ 数据通道不走本接口（那是控制/事件面，统一走消息信封，见
// jetson/common/msg_envelope.h）；本接口是数据面视频输出。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vox {

// 一个编码后的 H.264 访问单元（不含起始码，裸 Annex-B 或 AVCC 由实现方约定；
// 本项目统一 Annex-B 带 00 00 00 01 起始码，MP4/FLV 封装时再转长度前缀）
struct EncodedPacket {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool is_keyframe = false;
  int64_t timestamp_ns = 0;  // 对应源帧的采集时间戳
  uint64_t sequence = 0;     // 对应源帧序号
};

class IVideoSink {
 public:
  virtual ~IVideoSink() = default;

  // 编码器初始化完成后调用一次，下发 SPS/PPS extradata（Annex-B）。
  // MP4 的 avcC 封装与 RTMP 的 AVC sequence header 都需要它。
  virtual bool start(const std::vector<uint8_t>& sps_pps) = 0;

  // 编码线程直接回调；实现必须非阻塞且自行保证线程安全
  //（慢消费者在实现内部排队/丢帧，不得反压编码线程）。
  virtual void on_packet(const EncodedPacket& pkt) = 0;

  virtual void stop() = 0;
  virtual std::string name() const = 0;
};

}  // namespace vox
