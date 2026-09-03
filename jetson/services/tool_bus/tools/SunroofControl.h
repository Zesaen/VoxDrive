// SunroofControl.h
#pragma once
#include "../ToolHandler.h"
class SunroofControl : public ToolHandler {
public:
    explicit SunroofControl(StateManager& s) : ToolHandler(s) {}
    std::string execute(const std::string& action,
                        const std::unordered_map<std::string, std::string>&) override {
        if (action == "open") { state_.set("sunroof_state", "open"); return "天窗已打开"; }
        if (action == "close") { state_.set("sunroof_state", "closed"); return "天窗已关闭"; }
        if (action == "tilt") { state_.set("sunroof_state", "tilted"); return "天窗已翘起"; }
        return "天窗指令已执行";
    }
};

