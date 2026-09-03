// selftest_common.cpp — 公共地基自测：vox_config / vox_log / msg_envelope / json.hpp
// 编译：g++ -std=c++17 -I<jetson>/common selftest_common.cpp -o selftest_common
// 通过输出：SELFTEST_COMMON PASS
#define VOX_LOG_TAG "selftest"
#include "vox_config.h"
#include "vox_log.h"
#include "msg_envelope.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  // --- 配置装载与读取 ---
  assert(vox::config::load());
  assert(vox::config::has("port.tool_bus"));
  assert(vox::config::get_int("port.tool_bus", 0) == 6669);
  assert(vox::config::get("bind.host", "") == "*");
  assert(vox::config::get("nonexistent.key", "fb") == "fb");

  // --- 端点构造 ---
  assert(vox::config::bind_endpoint("port.tool_bus", "6699") == "tcp://*:6669");
  assert(vox::config::connect_endpoint("port.rag", "6670") == "tcp://localhost:6667");

  // --- $HOME 展开（模型路径键）---
  const std::string model = vox::config::get("llm.model", "");
  assert(!model.empty() && model[0] == '/');  // $HOME 已展开为绝对路径

  // --- JSON（nlohmann）---
  auto j = nlohmann::json::parse(R"({"name":"tool_bus","ports":[6669,6670]})");
  assert(j["ports"].size() == 2 && j["ports"][0] == 6669);

  // --- 消息信封 ---
  auto env = vox::msg::make(vox::msg::kTypeEvent, "selftest", {{"ok", true}, {"code", 42}});
  assert(vox::msg::valid(env));
  assert(env["version"] == 1 && env["type"] == "event");
  assert(env["payload"]["ok"] == true && env["payload"]["code"] == 42);
  assert(env["timestamp_ms"] > 1700000000000LL);  // 毫秒级墙钟

  // --- 日志（肉眼核对格式）---
  VOX_DEBUG("这条默认级别下不应出现");
  VOX_INFO("配置 %zu 键，端点 %s", vox::config::store().size(),
           vox::config::bind_endpoint("port.tool_bus", "").c_str());
  VOX_WARN("格式样例 warn");
  VOX_ERROR("格式样例 error");

  std::printf("SELFTEST_COMMON PASS\n");
  return 0;
}
