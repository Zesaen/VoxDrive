#pragma once
// StateManager — 车控状态集中管理（键值均为字符串，供各工具读写）
//
// JSON 输出约定（沿用 prj1 语义，dashboard 按此解析）：
//   "true"/"false" 输出为布尔；整数字符串输出为数字；其余输出为字符串。
// drain_changes_json() 取走自上次调用以来的增量（用于 state_change 广播与应答 state 字段）。
#include "../../common/json.hpp"

#include <string>
#include <unordered_map>

class StateManager {
public:
    StateManager() {
        state_["ac_on"] = "false";
        state_["ac_temp"] = "24";
        state_["ac_mode"] = "vent";
        state_["ac_fan"] = "2";
        state_["window_fl"] = "0";
        state_["window_fr"] = "0";
        state_["window_rl"] = "0";
        state_["window_rr"] = "0";
        state_["sunroof_state"] = "closed";
        state_["seat_driver"] = "0";
        state_["seat_passenger"] = "0";
        state_["camera_view"] = "front";
        state_["camera_recording"] = "false";
    }

    std::string get(const std::string& key) const {
        auto it = state_.find(key);
        return it != state_.end() ? it->second : "";
    }

    void set(const std::string& key, const std::string& value) {
        state_[key] = value;
        changed_[key] = value;
    }

    void set_int(const std::string& key, int value) {
        set(key, std::to_string(value));
    }

    int get_int(const std::string& key) const {
        auto it = state_.find(key);
        try {
            return it != state_.end() ? std::stoi(it->second) : 0;
        } catch (...) {
            return 0;
        }
    }

    nlohmann::json drain_changes_json() {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [k, v] : changed_) j[k] = to_json_value(v);
        changed_.clear();
        return j;
    }

    nlohmann::json to_json() const {
        nlohmann::json j = nlohmann::json::object();
        for (const auto& [k, v] : state_) j[k] = to_json_value(v);
        return j;
    }

private:
    static nlohmann::json to_json_value(const std::string& v) {
        if (v == "true") return true;
        if (v == "false") return false;
        try {
            size_t pos = 0;
            long iv = std::stol(v, &pos);
            if (pos == v.size()) return iv;
        } catch (...) {
        }
        return v;
    }

    std::unordered_map<std::string, std::string> state_;
    std::unordered_map<std::string, std::string> changed_;
};
