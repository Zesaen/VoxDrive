// ClimateControl.h
#pragma once
#include "../ToolHandler.h"
class ClimateControl : public ToolHandler {
public:
    explicit ClimateControl(StateManager& s) : ToolHandler(s) {}
    std::string execute(const std::string& action,
                        const std::unordered_map<std::string, std::string>& params) override {
        if (action == "on" || action == "turn_on") {
            state_.set("ac_on", "true");
            return "空调已打开";
        }
        if (action == "off" || action == "turn_off") {
            state_.set("ac_on", "false");
            return "空调已关闭";
        }
        if (action == "set_temp") {
            int t = param_int(params, "temp", 22);
            state_.set_int("ac_temp", t);
            state_.set("ac_on", "true");
            std::string mode = state_.get("ac_mode");
            return "空调已设置为" + std::to_string(t) + "度" +
                   (mode == "cool" ? "制冷" : mode == "heat" ? "制热" : "通风") + "模式";
        }
        if (action == "set_mode") {
            std::string m = params.count("mode") ? params.at("mode") : "cool";
            state_.set("ac_mode", m);
            state_.set("ac_on", "true");
            std::string mode_name = (m == "cool" ? "制冷" : m == "heat" ? "制热" : "通风"); return "空调模式已切换为" + mode_name;
        }
        if (action == "set_fan") {
            int f = param_int(params, "level", 3);
            state_.set_int("ac_fan", f);
            state_.set("ac_on", "true");
            return "风量已调到" + std::to_string(f) + "档";
        }
        return "空调指令已执行";
    }
};

