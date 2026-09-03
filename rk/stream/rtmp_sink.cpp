// rtmp_sink.cpp — RTMP 推流实现（flv over rtmp，libavformat）
#define VOX_LOG_TAG "rk.rtmp"

#include "rtmp_sink.h"

#include <sys/time.h>

#include <cstdio>

#include "json.hpp"
#include "vox/h264_nalu.h"
#include "vox_log.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace vox {
namespace {

int64_t wall_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

}  // namespace

RtmpSink::RtmpSink(const Params& p) : params_(p) {}

RtmpSink::~RtmpSink() { stop(); }

bool RtmpSink::start(const std::vector<uint8_t>& sps_pps) {
  if (params_.url.empty()) return false;
  avformat_network_init();
  avcc_ = annexb_to_avcc(sps_pps);
  if (avcc_.empty()) return false;
  avpkt_ = av_packet_alloc();
  if (!avpkt_) return false;
  active_ = true;
  // 首连放行（不阻塞 start）：连不上由 on_packet 里的 I 帧+冷却节奏重试
  last_connect_try_ms_ = wall_ms() - params_.reconnect_interval_ms;
  VOX_INFO("ready: url=%s %ux%u@%u（connect timeout %dms, retry %dms）",
           params_.url.c_str(), params_.width, params_.height, params_.fps,
           params_.connect_timeout_ms, params_.reconnect_interval_ms);
  return true;
}

bool RtmpSink::open_output() {
  AVFormatContext* f = nullptr;
  if (avformat_alloc_output_context2(&f, nullptr, "flv", params_.url.c_str()) < 0 || !f) {
    VOX_ERROR("创建 flv 输出上下文失败");
    return false;
  }
  AVStream* vs = avformat_new_stream(f, nullptr);
  if (!vs) {
    avformat_free_context(f);
    return false;
  }
  AVCodecParameters* par = vs->codecpar;
  par->codec_type = AVMEDIA_TYPE_VIDEO;
  par->codec_id = AV_CODEC_ID_H264;  // MPP 已编码，此处只封装
  par->width = static_cast<int>(params_.width);
  par->height = static_cast<int>(params_.height);
  par->format = AV_PIX_FMT_YUV420P;
  par->extradata = static_cast<uint8_t*>(
      av_malloc(avcc_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!par->extradata) {
    avformat_free_context(f);
    return false;
  }
  memcpy(par->extradata, avcc_.data(), avcc_.size());
  memset(par->extradata + avcc_.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
  par->extradata_size = static_cast<int>(avcc_.size());
  vs->time_base = AVRational{1, static_cast<int>(params_.fps)};  // flvenc 写 header 时改为 1/1000

  // 连接与 IO 超时（rw_timeout 通用于 URLContext；timeout 面向 tcp 连接）
  AVDictionary* opts = nullptr;
  const int64_t t_us = static_cast<int64_t>(params_.connect_timeout_ms) * 1000;
  av_dict_set_int(&opts, "rw_timeout", t_us, 0);
  av_dict_set_int(&opts, "timeout", t_us, 0);
  const int io_rc = (f->oformat->flags & AVFMT_NOFILE)
                        ? 0
                        : avio_open2(&f->pb, params_.url.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
  av_dict_free(&opts);
  if (io_rc < 0) {
    char err[256] = {0};
    av_strerror(io_rc, err, sizeof(err));
    VOX_WARN("连接 %s 失败: %s", params_.url.c_str(), err);
    if (!(f->oformat->flags & AVFMT_NOFILE) && f->pb) avio_closep(&f->pb);
    avformat_free_context(f);
    return false;
  }
  if (avformat_write_header(f, nullptr) < 0) {
    VOX_WARN("写 RTMP header 失败: %s", params_.url.c_str());
    if (!(f->oformat->flags & AVFMT_NOFILE) && f->pb) avio_closep(&f->pb);
    avformat_free_context(f);
    return false;
  }
  fmt_ = f;
  vstream_ = vs;
  uint64_t nth = 0;
  {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.connected = true;
    stats_.connect_count++;
    nth = stats_.connect_count;
  }
  VOX_INFO("connected: %s（第 %llu 次）", params_.url.c_str(),
           static_cast<unsigned long long>(nth));
  if (handler_) {
    handler_("rtmp_connected",
             nlohmann::json{{"url", params_.url}}.dump());
  }
  return true;
}

void RtmpSink::close_output() {
  if (!fmt_) return;
  if (fmt_->pb) {
    av_write_trailer(fmt_);  // 尽力收尾；连接已断时会失败，忽略
    avio_closep(&fmt_->pb);
  }
  avformat_free_context(fmt_);  // 释放流与 extradata
  fmt_ = nullptr;
  vstream_ = nullptr;
  {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.connected = false;
  }
}

void RtmpSink::on_packet(const EncodedPacket& pkt) {
  if (!active_) return;

  // 未连接：只在 I 帧 + 冷却期满时尝试重连（其余帧丢弃，接入流从 IDR 起可解码）
  if (!fmt_) {
    if (!pkt.is_keyframe) return;
    const int64_t now = wall_ms();
    if (now - last_connect_try_ms_ < params_.reconnect_interval_ms) return;
    last_connect_try_ms_ = now;
    if (!open_output()) {
      uint64_t fails = 0;
      {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.connect_failures++;
        fails = stats_.connect_failures;
      }
      if (handler_) {
        handler_("rtmp_connect_failed",
                 nlohmann::json{{"url", params_.url},
                                {"connect_failures", fails}}
                     .dump());
      }
      return;
    }
  }

  // FLV sample 与 MP4 同款 AVCC 转换（flvenc 不代转 Annex-B）
  scratch_.clear();
  annexb_to_length_prefixed(pkt.data, pkt.size, scratch_);
  if (scratch_.empty()) return;
  avpkt_->stream_index = vstream_->index;
  avpkt_->flags = pkt.is_keyframe ? AV_PKT_FLAG_KEY : 0;
  avpkt_->data = scratch_.data();
  avpkt_->size = static_cast<int>(scratch_.size());
  // PTS 以已发送帧数计（1/fps 基准再 rescale 到流时基），Baseline 无 B 帧 dts==pts
  avpkt_->pts = frame_index_;
  avpkt_->dts = frame_index_;
  av_packet_rescale_ts(avpkt_, AVRational{1, static_cast<int>(params_.fps)},
                       vstream_->time_base);

  if (av_interleaved_write_frame(fmt_, avpkt_) < 0) {
    uint64_t errs = 0;
    {
      std::lock_guard<std::mutex> lk(stats_mu_);
      stats_.write_errors++;
      errs = stats_.write_errors;
    }
    VOX_WARN("写流失败，断开连接待下个 I 帧重连（累计写失败 %llu）",
             static_cast<unsigned long long>(errs));
    if (handler_) {
      handler_("rtmp_disconnected",
               nlohmann::json{{"url", params_.url},
                              {"write_errors", errs}}
                   .dump());
    }
    close_output();
    return;
  }
  frame_index_++;
  {
    std::lock_guard<std::mutex> lk(stats_mu_);
    stats_.frames_sent++;
  }
}

RtmpSink::Stats RtmpSink::stats_snapshot() const {
  std::lock_guard<std::mutex> lk(stats_mu_);
  return stats_;
}

void RtmpSink::stop() {
  if (!active_) return;  // 幂等：显式 stop 后析构再调为无操作
  active_ = false;
  close_output();
  if (avpkt_) {
    av_packet_free(&avpkt_);
    avpkt_ = nullptr;
  }
  const Stats st = stats_snapshot();
  VOX_INFO("stopped: 推流 %llu 帧 / 建连 %llu 次 / 连接失败 %llu 次 / 写失败 %llu 次",
           static_cast<unsigned long long>(st.frames_sent),
           static_cast<unsigned long long>(st.connect_count),
           static_cast<unsigned long long>(st.connect_failures),
           static_cast<unsigned long long>(st.write_errors));
}

}  // namespace vox
