// mp4_segment_sink.h — 分段循环 MP4 录像 sink（R3）
//
// 职责：把 IVideoSink 送来的 H.264 访问单元按时间分段封装为 MP4 文件；
// 磁盘水位超限时按"最旧优先"删除已关闭的段（永不删正在写的当前段）；
// 写失败（磁盘满等）时关闭当前段、在下一个 I 帧处重开新段（断链恢复）。
// 分段边界只在 I 帧上滚动，保证每段可独立解码。
// 事件锁录（R10）：lock_current() 标记当前段，关闭时改 LOCK_ 前缀——
// 水位删除只扫 seg_ 前缀，LOCK_ 段天然豁免；锁段总量超配额时释放最旧锁段。
// 封装走 libavformat（板上 rkmpp 4.4.2 运行库 + 本地前缀头文件，
// 见 rk/scripts/setup_ffmpeg_headers.sh）。
#pragma once

#include <atomic>
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

class Mp4SegmentSink final : public IVideoSink {
 public:
  struct Params {
    std::string dir;                  // 录像目录（$HOME 已展开），自动创建
    uint32_t segment_seconds = 60;    // 单段目标时长
    uint32_t watermark_percent = 85;  // 磁盘使用率阈值，超过删最旧段
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    uint64_t lock_quota_bytes = 500ull * 1024 * 1024;  // 锁段总量上限（0=不限制；超出释放最旧锁段）
  };

  struct Stats {
    uint64_t segments_written = 0;  // 完整落盘的段数（含 stop 时收尾的当前段）
    uint64_t frames_written = 0;
    uint64_t bytes_written = 0;     // 全部段写入的裸流字节
    uint64_t segments_deleted = 0;  // 水位淘汰的段数
    uint64_t segments_locked = 0;   // 累计加锁段数（事件锁录）
    uint32_t locked_segments = 0;   // 当前锁段数（LOCK_ 前缀存量）
    uint64_t locked_bytes = 0;      // 当前锁段总字节
    std::string current_file;       // 正在写的段文件路径（空=无打开段）
  };

  explicit Mp4SegmentSink(const Params& p);
  ~Mp4SegmentSink() override;

  Mp4SegmentSink(const Mp4SegmentSink&) = delete;
  Mp4SegmentSink& operator=(const Mp4SegmentSink&) = delete;

  bool start(const std::vector<uint8_t>& sps_pps) override;
  void on_packet(const EncodedPacket& pkt) override;
  void stop() override;
  std::string name() const override { return "mp4_segment"; }

  // 事件回调（on_packet 调用线程内执行，须快速返回）：
  //   segment_opened / segment_closed / watermark_deleted / write_error
  //   segment_locked / lock_released（R10 事件锁录）
  // detail 为 JSON 字符串（file / used_percent 等字段）
  using EventHandler = std::function<void(const char* event, const std::string& detail)>;
  void set_event_handler(EventHandler h) { handler_ = std::move(h); }

  // 事件锁录（R10）：标记"当前正在写的段"在关闭时加锁（任意线程可调）。
  // 语义：事件发生时刻所属的段获得保护；段在下一个 I 帧边界关闭时改名 LOCK_。
  // 若此刻无打开段（断链恢复窗口），标记作用于下一段——事件窗口内可接受。
  void lock_current(const std::string& reason);

  const Stats& stats() const { return stats_; }          // 仅管线线程内使用
  Stats stats_snapshot() const;  // 跨线程快照（控制/查询线程用）

 private:
  bool open_segment();       // 以墙钟命名新段并写 header
  void close_segment();      // 写 trailer 并落盘（含待锁段的 LOCK_ 改名）
  void enforce_watermark();  // 超水位删最旧段（跳过当前段与全部 LOCK_ 段）
  void enforce_lock_quota();  // 锁段总量超配额时释放最旧锁段

  Params params_;
  std::vector<uint8_t> avcc_;  // start 时生成，每段复用
  AVFormatContext* fmt_ = nullptr;
  AVStream* vstream_ = nullptr;
  AVPacket* avpkt_ = nullptr;
  std::vector<uint8_t> scratch_;  // AVCC 转换复用缓冲（on_packet 内部使用）
  int64_t seg_base_ns_ = 0;   // 当前段首帧采集时间戳（段内 PTS 归零基准）
  bool await_keyframe_ = true;  // 段必须从 I 帧开始（含断链恢复后的重开）
  bool active_ = false;         // start 成功且未 stop（stop 幂等，析构安全重复调用）
  std::string current_path_;
  Stats stats_;
  mutable std::mutex stats_mu_;  // 保护 stats_ 的跨线程快照（写点在管线线程）
  EventHandler handler_;         // 可选事件回调（C5 PUB 上行）
  std::atomic<bool> lock_pending_{false};  // 检测线程置位，管线线程在段关闭时消费
  std::mutex lock_mu_;                     // 保护 lock_reason_
  std::string lock_reason_;
};

}  // namespace vox
