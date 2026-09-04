// mp4_segment_sink.cpp — 分段循环 MP4 录像实现
#define VOX_LOG_TAG "rk.storage"

#include "mp4_segment_sink.h"

#include <dirent.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "json.hpp"
#include "vox/h264_nalu.h"
#include "vox_log.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace vox {
namespace {

int64_t wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 文件名 seg_YYYYmmdd_HHMMSS.mp4，字典序即时间序（水位淘汰按名排序取最旧）
std::string segment_path(const std::string& dir) {
  char ts[24];
  time_t sec = wall_ns() / 1000000000;
  struct tm tmv;
  localtime_r(&sec, &tmv);
  strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tmv);
  return dir + "/seg_" + ts + ".mp4";
}

std::vector<std::string> list_segments(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string n(e->d_name);
    // seg_YYYYmmdd_HHMMSS.mp4：前缀+后缀匹配（名字含时间戳，字典序即时间序）
    if (n.size() > 8 && n.compare(0, 4, "seg_") == 0 &&
        n.compare(n.size() - 4, 4, ".mp4") == 0) {
      out.push_back(dir + "/" + n);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// 锁段列表（LOCK_ 前缀；水位删除只扫 seg_ 前缀，锁段天然豁免）
std::vector<std::string> list_locked(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = opendir(dir.c_str());
  if (!d) return out;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    std::string n(e->d_name);
    if (n.size() > 13 && n.compare(0, 9, "LOCK_seg_") == 0 &&
        n.compare(n.size() - 4, 4, ".mp4") == 0) {
      out.push_back(dir + "/" + n);
    }
  }
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

uint64_t file_size_of(const std::string& path) {
  std::error_code ec;
  const uint64_t sz = std::filesystem::file_size(path, ec);
  return ec ? 0 : sz;
}

}  // namespace

Mp4SegmentSink::Mp4SegmentSink(const Params& p) : params_(p) {}

Mp4SegmentSink::~Mp4SegmentSink() { stop(); }

bool Mp4SegmentSink::start(const std::vector<uint8_t>& sps_pps) {
  std::error_code ec;
  std::filesystem::create_directories(params_.dir, ec);
  if (ec && !std::filesystem::is_directory(params_.dir)) {
    VOX_ERROR("创建录像目录 %s 失败: %s", params_.dir.c_str(), ec.message().c_str());
    return false;
  }
  avcc_ = annexb_to_avcc(sps_pps);
  if (avcc_.empty()) return false;
  avpkt_ = av_packet_alloc();
  if (!avpkt_) return false;
  await_keyframe_ = true;  // 段从 I 帧开始
  active_ = true;
  VOX_INFO("ready: dir=%s segment=%us watermark=%u%%", params_.dir.c_str(),
           params_.segment_seconds, params_.watermark_percent);
  return true;
}

bool Mp4SegmentSink::open_segment() {
  current_path_ = segment_path(params_.dir);
  int rc = avformat_alloc_output_context2(&fmt_, nullptr, nullptr, current_path_.c_str());
  if (rc < 0 || !fmt_) {
    VOX_ERROR("avformat_alloc_output_context2(%s) rc=%d", current_path_.c_str(), rc);
    fmt_ = nullptr;
    return false;
  }
  vstream_ = avformat_new_stream(fmt_, nullptr);
  if (!vstream_) {
    VOX_ERROR("avformat_new_stream 失败");
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    return false;
  }
  AVCodecParameters* par = vstream_->codecpar;
  par->codec_type = AVMEDIA_TYPE_VIDEO;
  par->codec_id = AV_CODEC_ID_H264;
  par->format = AV_PIX_FMT_YUV420P;
  par->width = static_cast<int>(params_.width);
  par->height = static_cast<int>(params_.height);
  par->extradata_size = static_cast<int>(avcc_.size());
  par->extradata = static_cast<uint8_t*>(av_memdup(avcc_.data(), avcc_.size()));
  vstream_->time_base = AVRational{1, 90000};

  rc = avio_open(&fmt_->pb, current_path_.c_str(), AVIO_FLAG_WRITE);
  if (rc < 0) {
    VOX_ERROR("avio_open(%s) rc=%d", current_path_.c_str(), rc);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    return false;
  }
  rc = avformat_write_header(fmt_, nullptr);
  if (rc < 0) {
    VOX_ERROR("avformat_write_header rc=%d", rc);
    avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.current_file = current_path_;
  }
  enforce_watermark();  // 每段开启即查水位（与按帧数复查互补，段开始是自然的清理点）
  VOX_INFO("segment open: %s", current_path_.c_str());
  if (handler_) handler_("segment_opened", nlohmann::json{{"file", current_path_}}.dump());
  return true;
}

void Mp4SegmentSink::close_segment() {
  if (!fmt_) return;
  av_write_trailer(fmt_);
  avio_closep(&fmt_->pb);
  avformat_free_context(fmt_);  // 释放流与 extradata
  fmt_ = nullptr;
  vstream_ = nullptr;
  {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.segments_written++;
    stats_.current_file.clear();
  }
  VOX_INFO("segment closed: %s（累计 %llu 段）", current_path_.c_str(),
           static_cast<unsigned long long>(stats_.segments_written));
  if (handler_) {
    handler_("segment_closed",
             nlohmann::json{{"file", current_path_},
                            {"segments_total", stats_.segments_written},
                            {"bytes_total", stats_.bytes_written}}
                 .dump());
  }

  // 事件锁录（R10）：消费待锁标记 → LOCK_ 前缀改名（水位删除即豁免）
  if (lock_pending_.exchange(false)) {
    std::string reason;
    {
      std::lock_guard<std::mutex> lk(lock_mu_);
      reason = std::move(lock_reason_);
      lock_reason_.clear();
    }
    std::error_code ec;
    const std::filesystem::path from(current_path_);
    const std::filesystem::path to =
        from.parent_path() / ("LOCK_" + from.filename().string());
    std::filesystem::rename(from, to, ec);
    if (!ec) {
      const uint64_t sz = file_size_of(to.string());
      {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.segments_locked++;
        stats_.locked_segments++;
        stats_.locked_bytes += sz;
      }
      VOX_INFO("segment locked: %s（原因: %s，锁段存量 %u）", to.string().c_str(),
               reason.c_str(), [&] {
                 std::lock_guard<std::mutex> lk(stats_mu_);
                 return stats_.locked_segments;
               }());
      if (handler_) {
        handler_("segment_locked",
                 nlohmann::json{{"file", to.string()}, {"reason", reason}}.dump());
      }
      enforce_lock_quota();
    } else {
      VOX_WARN("锁段改名失败 %s: %s", to.string().c_str(), ec.message().c_str());
    }
  }
}

void Mp4SegmentSink::lock_current(const std::string& reason) {
  lock_pending_.store(true);
  std::lock_guard<std::mutex> lk(lock_mu_);
  lock_reason_ = reason;
}

void Mp4SegmentSink::enforce_lock_quota() {
  if (params_.lock_quota_bytes == 0) return;  // 0=不限制
  for (int iter = 0; iter < 64; ++iter) {  // 有界：每释放一个复查一次
    uint64_t locked_bytes = 0;
    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      locked_bytes = stats_.locked_bytes;
    }
    if (locked_bytes <= params_.lock_quota_bytes) return;
    std::vector<std::string> locked = list_locked(params_.dir);
    if (locked.empty()) return;
    const std::filesystem::path from(locked.front());
    const std::filesystem::path to =
        from.parent_path() / from.filename().string().substr(5);  // 去 LOCK_ 前缀
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec) return;
    const uint64_t sz = file_size_of(to.string());
    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      if (stats_.locked_segments > 0) stats_.locked_segments--;
      stats_.locked_bytes = stats_.locked_bytes > sz ? stats_.locked_bytes - sz : 0;
    }
    VOX_WARN("锁段超配额（%llu > %llu），释放最旧锁段 %s",
             static_cast<unsigned long long>(locked_bytes),
             static_cast<unsigned long long>(params_.lock_quota_bytes),
             to.string().c_str());
    if (handler_) {
      handler_("lock_released",
               nlohmann::json{{"file", to.string()},
                              {"quota_bytes", params_.lock_quota_bytes},
                              {"reason", "quota"}}
                   .dump());
    }
  }
}

void Mp4SegmentSink::enforce_watermark() {
  struct statvfs vfs {};
  if (statvfs(params_.dir.c_str(), &vfs) != 0 || vfs.f_blocks == 0) return;
  // 有界循环：每删一个最旧段复查一次水位（防异常状态下无限删）
  for (int iter = 0; iter < 64; ++iter) {
    const double used_pct = 100.0 * (1.0 - static_cast<double>(vfs.f_bavail) /
                                            static_cast<double>(vfs.f_blocks));
    if (used_pct < params_.watermark_percent) return;
    std::vector<std::string> segs = list_segments(params_.dir);
    if (!current_path_.empty() && !segs.empty() && segs.front() == current_path_) {
      segs.erase(segs.begin());  // 永不删正在写的当前段
    }
    if (segs.empty()) return;
    if (unlink(segs.front().c_str()) != 0) return;
    uint64_t deleted_total = 0;
    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      deleted_total = ++stats_.segments_deleted;
    }
    VOX_WARN("磁盘水位 %.0f%% ≥ %u%%，删除最旧段 %s（累计淘汰 %llu）", used_pct,
             params_.watermark_percent, segs.front().c_str(),
             static_cast<unsigned long long>(deleted_total));
    if (handler_) {
      handler_("watermark_deleted",
               nlohmann::json{{"file", segs.front()},
                              {"used_percent", used_pct},
                              {"segments_deleted", deleted_total}}
                   .dump());
    }
    if (statvfs(params_.dir.c_str(), &vfs) != 0) return;
  }
}

void Mp4SegmentSink::on_packet(const EncodedPacket& pkt) {
  if (await_keyframe_) {
    if (!pkt.is_keyframe) return;  // 重建段必须从 I 帧开始，中间帧丢弃
    await_keyframe_ = false;
    if (!open_segment()) {
      await_keyframe_ = true;
      return;
    }
    seg_base_ns_ = pkt.timestamp_ns;
  }

  // 分段滚动：I 帧边界 + 达到单段时长
  if (pkt.is_keyframe &&
      pkt.timestamp_ns - seg_base_ns_ >=
          static_cast<int64_t>(params_.segment_seconds) * 1000000000ll) {
    close_segment();
    if (!open_segment()) {
      await_keyframe_ = true;
      return;
    }
    seg_base_ns_ = pkt.timestamp_ns;
  }

  avpkt_->stream_index = vstream_->index;
  avpkt_->flags = pkt.is_keyframe ? AV_PKT_FLAG_KEY : 0;
  // 段内 PTS 从 0 起（Baseline 无 B 帧，dts==pts）
  const AVRational ns_tb{1, 1000000000};
  avpkt_->pts = av_rescale_q(pkt.timestamp_ns - seg_base_ns_, ns_tb, vstream_->time_base);
  avpkt_->dts = avpkt_->pts;

  // MP4 sample 必须是 AVCC（4 字节长度前缀），movenc 不代转 Annex-B——逐 NAL 重写
  scratch_.clear();
  annexb_to_length_prefixed(pkt.data, pkt.size, scratch_);
  if (scratch_.empty()) return;  // 无完整 NAL，丢弃
  avpkt_->data = scratch_.data();
  avpkt_->size = static_cast<int>(scratch_.size());

  if (av_interleaved_write_frame(fmt_, avpkt_) < 0) {
    VOX_ERROR("写帧失败（磁盘满/IO 错误），关闭当前段，待下个 I 帧重开（断链恢复）");
    if (handler_) {
      handler_("write_error", nlohmann::json{{"file", current_path_}}.dump());
    }
    close_segment();
    await_keyframe_ = true;
    return;
  }
  {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.frames_written++;
    stats_.bytes_written += pkt.size;
  }

  // 每写 ~10s 帧量复查一次水位（均摊开销）
  if (stats_.frames_written % (params_.fps * 10) == 0) enforce_watermark();
}

Mp4SegmentSink::Stats Mp4SegmentSink::stats_snapshot() const {
  std::lock_guard<std::mutex> lk(stats_mu_);
  return stats_;  // current_file 含 std::string，快照拷贝后锁即释放
}

void Mp4SegmentSink::stop() {
  if (!active_) return;  // 幂等：显式 stop 后析构再调为无操作
  active_ = false;
  close_segment();
  if (avpkt_) {
    av_packet_free(&avpkt_);
    avpkt_ = nullptr;
  }
  VOX_INFO("stopped: %llu 段 / %llu 帧 / %llu 字节 / 水位淘汰 %llu 段",
           static_cast<unsigned long long>(stats_.segments_written),
           static_cast<unsigned long long>(stats_.frames_written),
           static_cast<unsigned long long>(stats_.bytes_written),
           static_cast<unsigned long long>(stats_.segments_deleted));
}

}  // namespace vox
