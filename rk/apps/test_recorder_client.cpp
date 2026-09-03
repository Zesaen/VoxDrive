// test_recorder_client.cpp — C5/R5 服务自测客户端
//
// 验证 recorder_service 的两个通道：
//   1) REQ/REP：发信封查询 {"cmd":"status"}，断言应答信封合法、recording/pipeline_fps/
//      storage.segments_total 字段在；再发 set_recording=false → true 验证开关生效
//   2) PUB/SUB：订阅事件端口，短分段时长下 ~3s 内应收到 segment_* 事件信封
// 输出末行 "ZMQ_SERVICE_TEST PASS ..." / "FAIL ..."。
#define VOX_LOG_TAG "rk.test_client"

#include <zmq.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "ZmqClient.h"
#include "json.hpp"
#include "msg_envelope.h"
#include "vox_config.h"
#include "vox_log.h"

namespace {

std::string make_req(const std::string& cmd, nlohmann::json extra = {}) {
  nlohmann::json payload{{"cmd", cmd}};
  payload.merge(std::move(extra));
  return vox::msg::make(vox::msg::kTypeStatus, "test_client", payload).dump();
}

nlohmann::json query(zmq_component::ZmqClient& client, const std::string& body) {
  const std::string reply = client.request(body);
  return nlohmann::json::parse(reply, nullptr, false);
}

}  // namespace

int main() {
  if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，使用内置默认值");
  const std::string status_ep =
      "tcp://127.0.0.1:" + vox::config::get("rk.status_port", "6700");
  const std::string event_ep =
      "tcp://127.0.0.1:" + vox::config::get("rk.event_port", "6701");

  bool ok = true;

  // ---- 事件 SUB 先连接订阅（慢加入者：连接后等 0.5s 让订阅传播）----
  zmq::context_t ctx;
  zmq::socket_t sub(ctx, ZMQ_SUB);
  sub.set(zmq::sockopt::subscribe, "");
  sub.set(zmq::sockopt::rcvtimeo, 8000);
  sub.connect(event_ep);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  // ---- REQ/REP：状态查询 ----
  double fps = 0;
  {
    zmq_component::ZmqClient client(status_ep);
    client.setTimeout(5000);
    const nlohmann::json env = query(client, make_req("status"));
    const bool valid = vox::msg::valid(env) && env["type"] == vox::msg::kTypeStatus;
    const auto& p = valid ? env["payload"] : nlohmann::json::object();
    const bool fields = p.contains("recording") && p.contains("pipeline_fps") &&
                        p.contains("storage") && p["storage"].contains("segments_total");
    if (valid && fields) {
      fps = p["pipeline_fps"];
      VOX_INFO("status: recording=%d fps=%.1f segments=%llu used=%.0f%%",
               p["recording"].get<bool>() ? 1 : 0, fps,
               p["storage"]["segments_total"].get<uint64_t>(),
               p["storage"].value("used_percent", 0.0));
    } else {
      VOX_ERROR("status 应答不合法: %s", env.dump().c_str());
      ok = false;
    }

    // ---- 开关：off → 查询确认 → on 恢复 ----
    const nlohmann::json off = query(client, make_req("set_recording", {{"value", false}}));
    const nlohmann::json offst = query(client, make_req("status"));
    const nlohmann::json on = query(client, make_req("set_recording", {{"value", true}}));
    if (!(off["payload"]["recording"] == false && offst["payload"]["recording"] == false &&
          on["payload"]["recording"] == true)) {
      VOX_ERROR("录像开关应答异常");
      ok = false;
    } else {
      VOX_INFO("录像开关 off/on 生效");
    }
  }

  // ---- PUB/SUB：等事件（短分段下应收到 segment_opened/closed）----
  bool got_event = false;
  std::string event_name;
  zmq::message_t msg;
  if (sub.recv(msg)) {
    const nlohmann::json env =
        nlohmann::json::parse(std::string(static_cast<char*>(msg.data()), msg.size()),
                              nullptr, false);
    if (vox::msg::valid(env) && env["type"] == vox::msg::kTypeEvent) {
      got_event = true;
      event_name = env["payload"].value("event", "?");
      VOX_INFO("event: %s", event_name.c_str());
    }
  }
  if (!got_event) ok = false;

  const bool pass = ok && got_event && fps > 10.0;
  std::printf("ZMQ_SERVICE_TEST %s status_ok=%d recording_toggle=%d event=%s fps=%.1f\n",
              pass ? "PASS" : "FAIL", ok ? 1 : 0, ok ? 1 : 0,
              got_event ? event_name.c_str() : "none", fps);
  return pass ? 0 : 1;
}
