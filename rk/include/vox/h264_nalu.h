// h264_nalu.h — Annex-B 码流工具（MP4/FLV 封装共用，C3 抽取）
//
// 本项目编码输出统一为 Annex-B（00 00 00 01 起始码）；MP4(movenc) 与 FLV
// 均要求 AVCC 格式：extradata=avcC 记录、sample=NALU 4 字节长度前缀，
// 两个封装器都不代转，必须在这里显式转换。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vox {

struct NalRef {
  const uint8_t* p;
  size_t len;
};

// 扫描 Annex-B 缓冲里的全部 NAL。两个边界细节（都踩过）：
//   1) 起始码的前导零属于码本身不属于上一个 NAL——尾部零字节一律剔除
//      （RBSP 结束字节必非零，剔除零安全）；
//   2) 扫到缓冲区末尾时上一个 NAL 不能被截短（内层循环条件须含等号）。
std::vector<NalRef> split_annexb(const uint8_t* d, size_t n);

// Annex-B SPS/PPS → avcC 记录（MP4 extradata / FLV AVC sequence header 同款）。
// 失败返回空。
std::vector<uint8_t> annexb_to_avcc(const std::vector<uint8_t>& annexb);

// Annex-B 访问单元 → AVCC sample（逐 NAL 4 字节大端长度前缀），追加到 out。
void annexb_to_length_prefixed(const uint8_t* d, size_t n, std::vector<uint8_t>& out);

}  // namespace vox
