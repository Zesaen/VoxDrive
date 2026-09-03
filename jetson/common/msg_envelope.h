// msg_envelope.h — 跨板 ZMQ 消息信封（RK3588 ↔ Jetson）
//
// 统一信封：{"version":1, "type":"...", "timestamp_ms":..., "source":"...", "payload":{...}}
// 新增数据类型（GPS/IMU/检测事件/录像状态…）只需扩展 type，收发双方协议不变。
// REQ/REP 应答体与 PUB/SUB 事件体均使用本信封。
#pragma once

#include <cstdint>
#include <string>
#include <sys/time.h>
#include "json.hpp"

namespace vox {
namespace msg {

inline constexpr int kVersion = 1;

// 已定义类型（阶段 C/D 逐步启用，此清单即协议注册表）
inline constexpr const char* kTypeStatus = "status";    // 状态查询应答（录像/存储/水位）
inline constexpr const char* kTypeEvent = "event";      // 异常事件上行（断流/水位告警/锁录）
inline constexpr const char* kTypeSnapshot = "snapshot";  // 抓拍结果
// 预留：kTypeDetection（RKNN 检测事件，E2）、kTypeGps / kTypeImu（无硬件事不实现）

inline int64_t now_ms() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

inline nlohmann::json make(const std::string& type, const std::string& source,
                           nlohmann::json payload) {
  return nlohmann::json{
      {"version", kVersion},
      {"type", type},
      {"timestamp_ms", now_ms()},
      {"source", source},
      {"payload", std::move(payload)},
  };
}

// 信封合法性检查（version 兼容 + 必备字段）
inline bool valid(const nlohmann::json& env) {
  return env.is_object() && env.value("version", 0) == kVersion &&
         env.contains("type") && env.contains("timestamp_ms") && env.contains("payload");
}

}  // namespace msg
}  // namespace vox
