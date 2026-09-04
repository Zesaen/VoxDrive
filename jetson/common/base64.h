// base64.h — 极简 Base64 编解码（跨板 JPEG 抓拍传输用，两板共用）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vox {

inline const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string b64_encode(const uint8_t* data, size_t n) {
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = (data[i] << 16) |
                       (i + 1 < n ? data[i + 1] << 8 : 0) |
                       (i + 2 < n ? data[i + 2] : 0);
    out.push_back(kB64[(v >> 18) & 63]);
    out.push_back(kB64[(v >> 12) & 63]);
    out.push_back(i + 1 < n ? kB64[(v >> 6) & 63] : '=');
    out.push_back(i + 2 < n ? kB64[v & 63] : '=');
  }
  return out;
}

// 非法字符返回已解析的前缀（宽容解码，足够传输用）
inline std::vector<uint8_t> b64_decode(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> out;
  out.reserve(s.size() / 4 * 3);
  uint32_t buf = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    const int v = val(c);
    if (v < 0) continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
    }
  }
  return out;
}

}  // namespace vox
