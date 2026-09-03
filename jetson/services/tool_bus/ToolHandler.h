#pragma once
#include <string>
#include <unordered_map>
#include "StateManager.h"

class ToolHandler {
public:
    explicit ToolHandler(StateManager& state) : state_(state) {}
    virtual ~ToolHandler() = default;
    virtual std::string execute(const std::string& action,
                                const std::unordered_map<std::string, std::string>& params) = 0;

protected:
    StateManager& state_;
    int param_int(const std::unordered_map<std::string, std::string>& p,
                  const std::string& key, int def = 0) const {
        auto it = p.find(key);
        if (it == p.end()) return def;
        try {
            return std::stoi(it->second);
        } catch (...) {
            return def;
        }
    }
};
