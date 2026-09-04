// jpeg_encoder.cpp — NV12 → JPEG（libavcodec mjpeg + swscale，一次性编码）
#define VOX_LOG_TAG "rk.snapshot"

#include "jpeg_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include "vox_log.h"

namespace vox {

std::vector<uint8_t> encode_jpeg_nv12(const uint8_t* y, const uint8_t* uv,
                                      int width, int height,
                                      int stride_y, int stride_uv,
                                      int quality) {
  std::vector<uint8_t> out;
  if (!y || !uv || width <= 0 || height <= 0) return out;

  // NV12(带行距) → yuvj420p(mjpeg 全范围)，帧间无状态，逐次创建可接受
  SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_NV12,
                                   width, height, AV_PIX_FMT_YUVJ420P,
                                   SWS_POINT, nullptr, nullptr, nullptr);
  if (!sws) {
    VOX_ERROR("sws_getContext 失败");
    return out;
  }

  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!codec) {
    sws_freeContext(sws);
    VOX_ERROR("板上 ffmpeg 无 mjpeg 编码器");
    return out;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    sws_freeContext(sws);
    return out;
  }
  ctx->width = width;
  ctx->height = height;
  ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
  ctx->time_base = AVRational{1, 25};
  ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  // quality 1-100 → mjpeg qscale 2-31（global_quality 走 FF_QP2LAMBDA 标度）
  const int qscale = 2 + (100 - quality) * 29 / 99;
  ctx->global_quality = FF_QP2LAMBDA * qscale;
  ctx->flags |= AV_CODEC_FLAG_QSCALE;

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    VOX_ERROR("mjpeg avcodec_open2 失败");
    avcodec_free_context(&ctx);
    sws_freeContext(sws);
    return out;
  }

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_YUVJ420P;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame, 32) < 0) {
    VOX_ERROR("av_frame_get_buffer 失败");
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    sws_freeContext(sws);
    return out;
  }

  const uint8_t* src[2] = {y, uv};
  int src_stride[2] = {stride_y, stride_uv};
  sws_scale(sws, src, src_stride, 0, height, frame->data, frame->linesize);

  AVPacket* pkt = av_packet_alloc();
  if (avcodec_send_frame(ctx, frame) == 0) {
    while (avcodec_receive_packet(ctx, pkt) == 0) {
      out.insert(out.end(), pkt->data, pkt->data + pkt->size);
      av_packet_unref(pkt);
    }
  } else {
    VOX_ERROR("mjpeg send_frame 失败");
  }

  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&ctx);
  sws_freeContext(sws);
  return out;
}

}  // namespace vox
