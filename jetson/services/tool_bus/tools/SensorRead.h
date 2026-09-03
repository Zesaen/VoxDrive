#pragma once
#include "../ToolHandler.h"
#include "../../common/vox_config.h"

#include <cstdlib>
#include <fstream>

class SensorRead : public ToolHandler {
    std::string data_path_;

public:
    explicit SensorRead(StateManager& s) : ToolHandler(s) {
        // 优先级：SENSOR_DATA_FILE 环境变量（测试用）> conf toolbus.sensor_data_file > 相对默认
        if (const char* env = std::getenv("SENSOR_DATA_FILE")) {
            data_path_ = env;
        } else {
            data_path_ = vox::config::get("toolbus.sensor_data_file", "mock_sensor_data.txt");
        }
    }

    std::string execute(const std::string&,
                        const std::unordered_map<std::string, std::string>&) override {
        std::ifstream f(data_path_);
        if (!f.is_open()) return "{\"error\":\"file not found\"}";
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        // 去掉控制字符（换行等），保持单行 JSON
        std::string clean;
        for (char c : content)
            if (c >= 32) clean += c;
        return clean;
    }
};
