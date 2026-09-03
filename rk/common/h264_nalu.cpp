// h264_nalu.cpp — Annex-B 码流工具实现（自 mp4_segment_sink.cpp 原样抽取，语义不变）
#define VOX_LOG_TAG "rk.h264"

#include "vox/h264_nalu.h"

#include "vox_log.h"

namespace vox {

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

std::vector<uint8_t> annexb_to_avcc(const std::vector<uint8_t>& ab) {
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

void annexb_to_length_prefixed(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
  for (const NalRef& nal : split_annexb(d, n)) {
    out.push_back(static_cast<uint8_t>(nal.len >> 24));
    out.push_back(static_cast<uint8_t>(nal.len >> 16));
    out.push_back(static_cast<uint8_t>(nal.len >> 8));
    out.push_back(static_cast<uint8_t>(nal.len));
    out.insert(out.end(), nal.p, nal.p + nal.len);
  }
}

}  // namespace vox
