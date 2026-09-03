// mp4_segment_sink.h — 分段循环 MP4 录像 sink（R3）
//
// 职责：把 IVideoSink 送来的 H.264 访问单元按时间分段封装为 MP4 文件；
// 磁盘水位超限时按"最旧优先"删除已关闭的段（永不删正在写的当前段）；
// 写失败（磁盘满等）时关闭当前段、在下一个 I 帧处重开新段（断链恢复）。
// 分段边界只在 I 帧上滚动，保证每段可独立解码。
// 封装走 libavformat（板上 rkmpp 4.4.2 运行库 + 本地前缀头文件，
// 见 rk/scripts/setup_ffmpeg_headers.sh）。
#pragma once

#include <cstdint>
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
  };

  struct Stats {
    uint64_t segments_written = 0;  // 完整落盘的段数（含 stop 时收尾的当前段）
    uint64_t frames_written = 0;
    uint64_t bytes_written = 0;     // 全部段写入的裸流字节
    uint64_t segments_deleted = 0;  // 水位淘汰的段数
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

  // 状态查询（C5 ZMQ 服务 / R6 工具上报用）
  const Stats& stats() const { return stats_; }

 private:
  bool open_segment();       // 以墙钟命名新段并写 header
  void close_segment();      // 写 trailer 并落盘
  void enforce_watermark();  // 超水位删最旧段（跳过当前段）

  // Annex-B SPS/PPS → avcC（MP4 的 extradata 格式，含 4 字节长度前缀约定）
  static std::vector<uint8_t> annexb_to_avcc(const std::vector<uint8_t>& annexb);

  Params params_;
  std::vector<uint8_t> avcc_;  // start 时生成，每段复用
  AVFormatContext* fmt_ = nullptr;
  AVStream* vstream_ = nullptr;
  AVPacket* avpkt_ = nullptr;
  std::vector<uint8_t> scratch_;  // AVCC 转换复用缓冲（on_packet 内部使用）
  int64_t seg_base_ns_ = 0;   // 当前段首帧采集时间戳（段内 PTS 归零基准）
  bool await_keyframe_ = true;  // 段必须从 I 帧开始（含断链恢复后的重开）
  std::string current_path_;
  Stats stats_;
};

}  // namespace vox
