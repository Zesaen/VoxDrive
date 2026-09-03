// WindowControl.h
#pragma once
#include "../ToolHandler.h"
class WindowControl : public ToolHandler {
public:
    explicit WindowControl(StateManager& s) : ToolHandler(s) {}
    std::string execute(const std::string& action,
                        const std::unordered_map<std::string, std::string>& params) override {
        auto set_win = [&](const std::string& key, const std::string& name) -> std::string {
            int pct = param_int(params, "percent", action == "close" || action == "close_all" ? 0 : 100);
            state_.set_int(key, pct);
            return name + (pct > 0 ? "已打开" + std::to_string(pct) + "%" : "已关闭");
        };
        if (action == "open_all") {
            state_.set_int("window_fl", 100); state_.set_int("window_fr", 100);
            state_.set_int("window_rl", 100); state_.set_int("window_rr", 100);
            return "所有车窗已打开";
        }
        if (action == "close_all") {
            state_.set_int("window_fl", 0); state_.set_int("window_fr", 0);
            state_.set_int("window_rl", 0); state_.set_int("window_rr", 0);
            return "所有车窗已关闭";
        }
        if (action == "fl") return set_win("window_fl", "左前车窗");
        if (action == "fr") return set_win("window_fr", "右前车窗");
        if (action == "rl") return set_win("window_rl", "左后车窗");
        if (action == "rr") return set_win("window_rr", "右后车窗");
        if (action == "set") {
            if (params.count("fl")) state_.set_int("window_fl", param_int(params, "fl", 0));
            if (params.count("fr")) state_.set_int("window_fr", param_int(params, "fr", 0));
            if (params.count("rl")) state_.set_int("window_rl", param_int(params, "rl", 0));
            if (params.count("rr")) state_.set_int("window_rr", param_int(params, "rr", 0));
            return "车窗已调节";
        }
        return "车窗指令已执行";
    }
};

