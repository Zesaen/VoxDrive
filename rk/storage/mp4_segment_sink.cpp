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

struct NalRef {
  const uint8_t* p;
  size_t len;
};

// 扫描 Annex-B 装载里的全部 NAL。两个边界细节（都踩过）：
//   1) 起始码的前导零属于码本身不属于上一个 NAL——尾部零字节一律剔除
//      （RBSP 结束字节必非零，剔除零安全）；
//   2) 扫到缓冲区末尾时上一个 NAL 不能被截短（内层循环条件须含等号）。
std::vector<NalRef> split_annexb(const uint8_t* d, size_t n) {
  std::vector<NalRef> out;
  size_t i = 0;
  while (i + 4 <= n) {  // 起始码后至少 1 字节 NAL 头才有意义
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
      const size_t start = i + 3;
      size_t j = start;
      while (j + 3 <= n && !(d[j] == 0 && d[j + 1] == 0 && d[j + 2] == 1)) ++j;
      size_t end = (j + 3 <= n) ? j : n;
      while (end > start && d[end - 1] == 0) --end;
      if (end > start) out.push_back({d + start, end - start});
      i = j;
    } else {
      ++i;
    }
  }
  return out;
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

}  // namespace

Mp4SegmentSink::Mp4SegmentSink(const Params& p) : params_(p) {}

Mp4SegmentSink::~Mp4SegmentSink() { stop(); }

std::vector<uint8_t> Mp4SegmentSink::annexb_to_avcc(const std::vector<uint8_t>& ab) {
  const uint8_t* sps = nullptr;
  const uint8_t* pps = nullptr;
  size_t sps_len = 0, pps_len = 0;
  for (const NalRef& n : split_annexb(ab.data(), ab.size())) {
    const uint8_t type = n.p[0] & 0x1f;
    if (type == 7 && !sps) { sps = n.p; sps_len = n.len; }
    if (type == 8 && !pps) { pps = n.p; pps_len = n.len; }
  }
  if (!sps || !pps || sps_len < 4) {
    VOX_ERROR("extradata 中未找到 SPS/PPS（sps=%p pps=%p）", (void*)sps, (void*)pps);
    return {};
  }
  std::vector<uint8_t> avcc;
  avcc.reserve(11 + sps_len + pps_len);
  avcc.push_back(1);                                // configurationVersion
  avcc.push_back(sps[1]);                           // profile_idc
  avcc.push_back(sps[2]);                           // constraint flags
  avcc.push_back(sps[3]);                           // level_idc
  avcc.push_back(0xfc | 0x03);                      // lengthSizeMinusOne=3（4 字节长度前缀）
  avcc.push_back(0xe0 | 0x01);                      // numOfSequenceParameterSets=1
  avcc.push_back(static_cast<uint8_t>(sps_len >> 8));
  avcc.push_back(static_cast<uint8_t>(sps_len & 0xff));
  avcc.insert(avcc.end(), sps, sps + sps_len);
  avcc.push_back(0x01);                             // numOfPictureParameterSets
  avcc.push_back(static_cast<uint8_t>(pps_len >> 8));
  avcc.push_back(static_cast<uint8_t>(pps_len & 0xff));
  avcc.insert(avcc.end(), pps, pps + pps_len);
  return avcc;
}

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
  for (const NalRef& n : split_annexb(pkt.data, pkt.size)) {
    scratch_.push_back(static_cast<uint8_t>(n.len >> 24));
    scratch_.push_back(static_cast<uint8_t>(n.len >> 16));
    scratch_.push_back(static_cast<uint8_t>(n.len >> 8));
    scratch_.push_back(static_cast<uint8_t>(n.len));
    scratch_.insert(scratch_.end(), n.p, n.p + n.len);
  }
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
