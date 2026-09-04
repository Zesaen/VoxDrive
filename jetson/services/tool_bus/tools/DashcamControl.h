// DashcamControl.h — 行车记录仪跨板工具（D1/R6）
//
// 与本地工具不同：execute 把动作转成消息信封 ZMQ REQ 转发给 RK 的
// recorder_service（rk.ip:rk.status_port），应答信封解出 payload 组织
// 成给 LLM/TTS 的中文结果串；抓拍 JPEG 落盘到本板 snapshot 目录。
// RK 离线/超时返回明确话术（ok=false 由总线原样传递语义，这里以
// result 字符串表达，LLM 可据此向用户解释）。
#pragma once
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "../ToolHandler.h"
#include "ZmqClient.h"
#include "base64.h"
#include "json.hpp"
#include "msg_envelope.h"
#include "vox_config.h"

class DashcamControl : public ToolHandler {
public:
    explicit DashcamControl(StateManager& s) : ToolHandler(s) {}

    std::string execute(const std::string& action,
                        const std::unordered_map<std::string, std::string>&) override {
        // RK 地址 conf 化：网络形态变化不改代码
        const std::string ip = vox::config::get("rk.ip", "192.168.137.200");
        const std::string port = vox::config::get("rk.status_port", "6700");
        const std::string ep = "tcp://" + ip + ":" + port;

        zmq_component::ZmqClient client(ep);  // 每次调用新建连接（低频控制面，避免长连接状态）
        client.setTimeout(5000);

        const std::string req_type =
            (action == "snapshot") ? vox::msg::kTypeSnapshot : vox::msg::kTypeStatus;
        nlohmann::json payload;
        if (action == "status") {
            payload = {{"cmd", "status"}};
        } else if (action == "record_on" || action == "record_off") {
            payload = {{"cmd", "set_recording"}, {"value", action == "record_on"}};
        } else if (action == "preview_on" || action == "preview_off") {
            payload = {{"cmd", "set_preview"}, {"value", action == "preview_on"}};
        } else if (action == "snapshot") {
            payload = {{"cmd", "snapshot"}};
        } else {
            return "未知动作: " + action;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::string reply;
        try {
            reply = client.request(vox::msg::make(req_type, "jetson.tool_bus", payload).dump());
        } catch (const zmq_component::ZmqCommunicationError&) {
            return "行车记录仪离线（" + ip + " 无应答）";
        }
        const double rtt_ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();

        const nlohmann::json env = nlohmann::json::parse(reply, nullptr, false);
        if (!vox::msg::valid(env)) return "行车记录仪应答格式错误";
        const nlohmann::json& p = env["payload"];
        if (p.contains("error")) return "行车记录仪错误: " + p["error"].get<std::string>();

        if (action == "status") {
            // 组织给 LLM 的结构化结果（中文键对 TTS 友好，rtt 供 R9 对账）
            const auto& storage = p.value("storage", nlohmann::json::object());
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "正在录像:%s 预览:%s 帧率:%.1ffps 已录%llu段 "
                          "磁盘占用%.0f%% 剩余%.1fGB (rtt=%.0fms)",
                          p.value("recording", false) ? "是" : "否",
                          p.value("preview", false) ? "开" : "关",
                          p.value("pipeline_fps", 0.0),
                          static_cast<unsigned long long>(
                              storage.value("segments_total", 0)),
                          storage.value("used_percent", 0.0),
                          storage.value("free_gb", 0.0), rtt_ms);
            return buf;
        }
        if (action == "record_on" || action == "record_off") {
            const bool on = p.value("recording", false);
            state_.set("dashcam_recording", on ? "true" : "false");
            return std::string(on ? "录像已开启" : "录像已暂停");
        }
        if (action == "preview_on" || action == "preview_off") {
            const bool on = p.value("preview", false);
            state_.set("dashcam_preview", on ? "true" : "false");
            return std::string(on ? "预览推流已开启" : "预览推流已关闭");
        }

        // snapshot：JPEG base64 → 本板落盘
        const std::string b64 = p.value("jpeg_b64", "");
        if (b64.empty()) return "抓拍失败：无图像数据";
        const std::vector<uint8_t> jpeg = vox::b64_decode(b64);
        const std::string dir = vox::config::expand_home(
            vox::config::get("snapshot_dir", "$HOME/voxdrive_snapshots"));
        const std::string path = dir + "/snap_" +
                                 std::to_string(vox::msg::now_ms()) + ".jpg";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream f(path, std::ios::binary);
        if (!f) return "抓拍成功但保存失败: " + path;
        f.write(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
        char buf[256];
        std::snprintf(buf, sizeof(buf), "已抓拍 %dx%d 保存至 %s (%zuKB rtt=%.0fms)",
                      p.value("width", 0), p.value("height", 0), path.c_str(),
                      jpeg.size() / 1024, rtt_ms);
        return buf;
    }
};
