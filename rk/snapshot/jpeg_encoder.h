// jpeg_encoder.h — NV12 帧一次性 JPEG 编码（R6 抓拍）
//
// 抓拍频率极低（人发起），不走常驻编码器：每次调用完整建 swscale(mjpeg 收
// yuvj420p)+avcodec 上下文、编一帧、销毁。软件编码 1080p 约 50-100ms——
// 在采集管线线程内同步执行会造成单次 1-2 帧周期的延迟毛刺，由 V4L2 多缓冲
// 吸收（4 缓冲 ≈ 133ms 余量），抓拍场景可接受。
#pragma once

#include <cstdint>
#include <vector>

namespace vox {

// NV12 双平面 → JPEG。quality 1-100。失败返回空。
std::vector<uint8_t> encode_jpeg_nv12(const uint8_t* y, const uint8_t* uv,
                                      int width, int height,
                                      int stride_y, int stride_uv,
                                      int quality = 85);

}  // namespace vox
