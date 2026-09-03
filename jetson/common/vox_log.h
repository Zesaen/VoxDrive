// vox_log.h — VoxDrive 统一日志（header-only）
//
// 格式：2026-09-03 21:45:12.345 [INFO ] [tool_bus] 消息内容
//   - 毫秒时间戳：跨板/跨服务延迟对账的基础（两板 NTP 未部署前，跨板对账以同板内分段打点为主）
//   - 输出到 stderr，由启动脚本重定向到 /tmp/<service>.log
// 用法：在每个 .cpp include 本文件之前定义服务标签：
//   #define VOX_LOG_TAG "tool_bus"
//   #include "vox_log.h"
// 级别：环境变量 VOX_LOG（debug/info/warn/error）或配置 log.level，默认 info。
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/time.h>

#ifndef VOX_LOG_TAG
#define VOX_LOG_TAG "vox"
#endif

namespace vox {
namespace log {

inline int level_no() {
  static int cached = [] {
    int no = 1;  // info
    std::string lv;
    if (const char* e = std::getenv("VOX_LOG")) lv = e;
    if (lv.empty()) lv = "info";
    if (lv == "debug") no = 0;
    else if (lv == "info") no = 1;
    else if (lv == "warn") no = 2;
    else if (lv == "error") no = 3;
    return no;
  }();
  return cached;
}

inline void write(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
inline void write(const char* level, const char* fmt, ...) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tmv;
  localtime_r(&tv.tv_sec, &tmv);
  char head[24];  // "2026-09-03 21:45:12"
  std::strftime(head, sizeof(head), "%Y-%m-%d %H:%M:%S", &tmv);
  std::fprintf(stderr, "%s.%03ld [%-5s] [%s] ", head, tv.tv_usec / 1000, level, VOX_LOG_TAG);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
  std::fputc('\n', stderr);
}

}  // namespace log
}  // namespace vox

#define VOX_DEBUG(...) do { if (vox::log::level_no() <= 0) vox::log::write("DEBUG", __VA_ARGS__); } while (0)
#define VOX_INFO(...)  do { if (vox::log::level_no() <= 1) vox::log::write("INFO ", __VA_ARGS__); } while (0)
#define VOX_WARN(...)  do { if (vox::log::level_no() <= 2) vox::log::write("WARN ", __VA_ARGS__); } while (0)
#define VOX_ERROR(...) do { if (vox::log::level_no() <= 3) vox::log::write("ERROR", __VA_ARGS__); } while (0)
