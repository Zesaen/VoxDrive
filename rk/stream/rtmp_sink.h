// rtmp_sink.h — RTMP 推流 sink（C4/R4）
//
// 把 MPP 输出的 H.264 Annex-B 访问单元封装为 FLV over RTMP 推出：
//   extradata = avcC（FLV AVC sequence header，同 MP4 extradata 格式）
//   sample    = AVCC（4 字节长度前缀，与 MP4 同款转换，flvenc 不代转 Annex-B）
// 断链恢复（沿用 prj2 拉流 2s 超时/重连节奏）：写失败即关连接，回到
// 等待 I 帧状态，冷却期内跳过、冷却期满后重连；重连前丢弃非关键帧，
// 保证任何时刻接入的流都从 IDR 开始可解码。推流失败绝不影响录像管线
//（IVideoSink 契约：on_packet 不得抛出/终止调用方）。
// 封装走 libavformat（flv muxer + 原生 rtmp 协议），板上 rkmpp 4.4.2。
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "vox/video_sink.h"

typedef struct AVFormatContext AVFormatContext;
typedef struct AVStream AVStream;
typedef struct AVPacket AVPacket;

namespace vox {

class RtmpSink final : public IVideoSink {
 public:
  struct Params {
    std::string url;   // rtmp://host:port/app/stream（空=不启用）
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    int connect_timeout_ms = 5000;         // TCP 连接与 IO 超时
    int reconnect_interval_ms = 2000;      // 重连冷却
  };

  struct Stats {
    uint64_t frames_sent = 0;
    uint64_t connect_count = 0;     // 成功建连次数（含重连）
    uint64_t connect_failures = 0;  // 建连失败次数
    uint64_t write_errors = 0;      // 写失败次数（触发断链恢复）
    bool connected = false;
  };

  explicit RtmpSink(const Params& p);
  ~RtmpSink() override;

  RtmpSink(const RtmpSink&) = delete;
  RtmpSink& operator=(const RtmpSink&) = delete;

  bool start(const std::vector<uint8_t>& sps_pps) override;
  void on_packet(const EncodedPacket& pkt) override;
  void stop() override;
  std::string name() const override { return "rtmp"; }

  // 事件回调（on_packet 调用线程内执行，须快速返回）：
  //   rtmp_connected / rtmp_disconnected / rtmp_connect_failed
  // detail 为 JSON 字符串（url / error / attempt 等字段）
  using EventHandler = std::function<void(const char* event, const std::string& detail)>;
  void set_event_handler(EventHandler h) { handler_ = std::move(h); }

  Stats stats_snapshot() const;  // 跨线程快照

 private:
  bool open_output();   // 建 AVFormatContext 并发布（connect + header）
  void close_output();  // 写 trailer 并释放（幂等）

  Params params_;
  AVFormatContext* fmt_ = nullptr;
  AVStream* vstream_ = nullptr;
  AVPacket* avpkt_ = nullptr;
  std::vector<uint8_t> avcc_;        // start 时生成，重连复用
  std::vector<uint8_t> scratch_;     // AVCC sample 转换复用缓冲
  int64_t frame_index_ = 0;          // 已发送帧计数（PTS 基准，1/fps）
  bool await_keyframe_ = true;       // 未连接：等 I 帧再（重）连
  bool active_ = false;              // start 成功且未 stop
  int64_t last_connect_try_ms_ = 0;  // 重连冷却计时
  Stats stats_;
  mutable std::mutex stats_mu_;      // 保护 stats_ 的跨线程快照（写点在管线线程）
  EventHandler handler_;
};

}  // namespace vox
