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

}  // namespace

Mp4SegmentSink::Mp4SegmentSink(const Params& p) : params_(p) {}

Mp4SegmentSink::~Mp4SegmentSink() { stop(); }

std::vector<uint8_t> Mp4SegmentSink::annexb_to_avcc(const std::vector<uint8_t>& ab) {
  // 拆 NAL：匹配 00 00 01 起始码（兼容 3/4 字节），到下一个起始码为止
  std::vector<const uint8_t*> nals;
  std::vector<size_t> lens;
  size_t i = 0;
  const size_t n = ab.size();
  while (i + 3 < n) {
    if (ab[i] == 0 && ab[i + 1] == 0 && ab[i + 2] == 1) {
      size_t start = i + 3;
      size_t j = start;
      while (j + 3 < n && !(ab[j] == 0 && ab[j + 1] == 0 && ab[j + 2] == 1)) ++j;
      nals.push_back(ab.data() + start);
      lens.push_back(j - start);
      i = j;
    } else {
      ++i;
    }
  }
  const uint8_t* sps = nullptr;
  const uint8_t* pps = nullptr;
  size_t sps_len = 0, pps_len = 0;
  for (size_t k = 0; k < nals.size(); ++k) {
    const uint8_t type = nals[k][0] & 0x1f;
    if (type == 7 && !sps) { sps = nals[k]; sps_len = lens[k]; }
    if (type == 8 && !pps) { pps = nals[k]; pps_len = lens[k]; }
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
  stats_.current_file = current_path_;
  enforce_watermark();  // 每段开启即查水位（与按帧数复查互补，段开始是自然的清理点）
  VOX_INFO("segment open: %s", current_path_.c_str());
  return true;
}

void Mp4SegmentSink::close_segment() {
  if (!fmt_) return;
  av_write_trailer(fmt_);
  avio_closep(&fmt_->pb);
  avformat_free_context(fmt_);  // 释放流与 extradata
  fmt_ = nullptr;
  vstream_ = nullptr;
  stats_.segments_written++;
  stats_.current_file.clear();
  VOX_INFO("segment closed: %s（累计 %llu 段）", current_path_.c_str(),
           static_cast<unsigned long long>(stats_.segments_written));
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
    stats_.segments_deleted++;
    VOX_WARN("磁盘水位 %.0f%% ≥ %u%%，删除最旧段 %s（累计淘汰 %llu）", used_pct,
             params_.watermark_percent, segs.front().c_str(),
             static_cast<unsigned long long>(stats_.segments_deleted));
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

  avpkt_->data = const_cast<uint8_t*>(pkt.data);  // 借用指针，写入在本调用内完成
  avpkt_->size = static_cast<int>(pkt.size);
  avpkt_->stream_index = vstream_->index;
  avpkt_->flags = pkt.is_keyframe ? AV_PKT_FLAG_KEY : 0;
  // 段内 PTS 从 0 起（Baseline 无 B 帧，dts==pts）
  const AVRational ns_tb{1, 1000000000};
  avpkt_->pts = av_rescale_q(pkt.timestamp_ns - seg_base_ns_, ns_tb, vstream_->time_base);
  avpkt_->dts = avpkt_->pts;

  if (av_interleaved_write_frame(fmt_, avpkt_) < 0) {
    VOX_ERROR("写帧失败（磁盘满/IO 错误），关闭当前段，待下个 I 帧重开（断链恢复）");
    close_segment();
    await_keyframe_ = true;
    return;
  }
  stats_.frames_written++;
  stats_.bytes_written += pkt.size;

  // 每写 ~10s 帧量复查一次水位（均摊开销）
  if (stats_.frames_written % (params_.fps * 10) == 0) enforce_watermark();
}

void Mp4SegmentSink::stop() {
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
