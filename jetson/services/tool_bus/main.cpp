// tool_bus — 工具总线服务（REP 应答工具调用 / PUB 状态广播）
//
// 协议（与 prj1 兼容）：
//   请求  {"tool":"climate_control","action":"set_temp","temp":"22"}
//   应答  {"ok":true,"result":"...","state":{本次变更键值}}   （无变更省略 state）
//   广播  {"type":"state_full"|"state_change","tool":..,"action":..,"values":{..}}
//
// 工具注册表：本地工具（直接执行）+ 跨板工具 dashcam（ZMQ 转发 RK
// recorder_service，统一消息信封；状态查询/录像与预览开关/抓拍）。
#define VOX_LOG_TAG "tool_bus"
#include "../../common/vox_log.h"
#include "../../common/vox_config.h"
#include "../../common/json.hpp"

#include "StateManager.h"
#include "ToolHandler.h"
#include "ZmqPub.h"
#include "ZmqServer.h"
#include "tools/CameraCapture.h"
#include "tools/ClimateControl.h"
#include "tools/DashcamControl.h"
#include "tools/SeatHeater.h"
#include "tools/SensorRead.h"
#include "tools/SunroofControl.h"
#include "tools/WindowControl.h"

#include <memory>
#include <unordered_map>

namespace {

using Params = std::unordered_map<std::string, std::string>;

// 请求 JSON → 扁平参数表：字符串取字面值，数字/布尔取 dump 字面量（与旧版语义一致）
Params to_params(const nlohmann::json& req) {
    Params params;
    if (!req.is_object()) return params;
    for (auto it = req.begin(); it != req.end(); ++it) {
        params[it.key()] = it->is_string() ? it->get<std::string>() : it->dump();
    }
    return params;
}

std::string build_reply(bool ok, const std::string& result, const nlohmann::json& state) {
    nlohmann::json reply{{"ok", ok}, {"result", result}};
    if (!state.empty()) reply["state"] = state;
    return reply.dump();
}

}  // namespace

int main() {
    if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，端口/路径使用内置默认值");

    const std::string rep_ep = vox::config::bind_endpoint("port.tool_bus", "6669");
    const std::string pub_ep = vox::config::bind_endpoint("port.tool_bus_pub", "6670");

    zmq_component::ZmqServer rep(rep_ep);
    zmq_component::ZmqPub pub(pub_ep);

    StateManager state;

    std::unordered_map<std::string, std::unique_ptr<ToolHandler>> tools;
    tools["climate_control"] = std::make_unique<ClimateControl>(state);
    tools["window_control"]  = std::make_unique<WindowControl>(state);
    tools["sunroof_control"] = std::make_unique<SunroofControl>(state);
    tools["seat_heater"]     = std::make_unique<SeatHeater>(state);
    tools["camera_capture"]  = std::make_unique<CameraCapture>(state);
    tools["sensor_read"]     = std::make_unique<SensorRead>(state);
    tools["dashcam"]         = std::make_unique<DashcamControl>(state);  // 跨板：RK recorder_service

    pub.publish(nlohmann::json{{"type", "state_full"}, {"values", state.to_json()}}.dump());
    VOX_INFO("listening %s, pub %s（本地工具 %zu 个）", rep_ep.c_str(), pub_ep.c_str(),
             tools.size());

    while (true) {
        const std::string request = rep.receive();
        VOX_INFO("recv: %.120s", request.c_str());

        auto req = nlohmann::json::parse(request, nullptr, /*allow_exceptions=*/false);
        if (req.is_discarded()) {
            rep.send(build_reply(false, "malformed json", {}));
            continue;
        }

        const std::string tool_name = req.value("tool", "");
        const std::string action = req.value("action", "");
        if (tool_name.empty()) {
            rep.send(build_reply(false, "missing tool", {}));
            continue;
        }

        auto it = tools.find(tool_name);
        if (it == tools.end()) {
            rep.send(build_reply(false, "unknown tool: " + tool_name, {}));
            continue;
        }

        const std::string result = it->second->execute(action, to_params(req));
        const nlohmann::json state_changes = state.drain_changes_json();

        if (!state_changes.empty()) {
            pub.publish(nlohmann::json{{"type", "state_change"},
                                       {"tool", tool_name},
                                       {"action", action},
                                       {"values", state_changes}}
                            .dump());
        }

        rep.send(build_reply(true, result, state_changes));
        VOX_INFO("-> ok, result=%.60s", result.c_str());
    }
}
