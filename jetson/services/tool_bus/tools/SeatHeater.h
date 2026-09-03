// SeatHeater.h
#pragma once
#include "../ToolHandler.h"
class SeatHeater : public ToolHandler {
public:
    explicit SeatHeater(StateManager& s) : ToolHandler(s) {}
    std::string execute(const std::string& action,
                        const std::unordered_map<std::string, std::string>& params) override {
        int level = param_int(params, "level", 2);
        if (action == "driver_on") { state_.set_int("seat_driver", level); return "主驾座椅加热已开启" + std::to_string(level) + "档"; }
        if (action == "driver_off") { state_.set_int("seat_driver", 0); return "主驾座椅加热已关闭"; }
        if (action == "passenger_on") { state_.set_int("seat_passenger", level); return "副驾座椅加热已开启" + std::to_string(level) + "档"; }
        if (action == "passenger_off") { state_.set_int("seat_passenger", 0); return "副驾座椅加热已关闭"; }
        if (action == "on") {
            int dl = param_int(params, "driver_level", 2);
            int pl = param_int(params, "passenger_level", 2);
            state_.set_int("seat_driver", dl);
            state_.set_int("seat_passenger", pl);
            return "座椅加热已开启";
        }
        if (action == "off") { state_.set_int("seat_driver", 0); state_.set_int("seat_passenger", 0); return "座椅加热已全部关闭"; }
        return "座椅加热指令已执行";
    }
};

