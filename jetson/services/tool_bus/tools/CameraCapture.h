// CameraCapture.h
#pragma once
#include "../ToolHandler.h"
class CameraCapture : public ToolHandler {
public:
    explicit CameraCapture(StateManager& s) : ToolHandler(s) {}
    std::string execute(const std::string& action,
                        const std::unordered_map<std::string, std::string>&) override {
        if (action == "front")  { state_.set("camera_view", "front");  return "已切换到前视摄像头"; }
        if (action == "rear")   { state_.set("camera_view", "rear");   return "已切换到后视摄像头"; }
        if (action == "left")   { state_.set("camera_view", "left");   return "已切换到左侧摄像头"; }
        if (action == "right")  { state_.set("camera_view", "right");  return "已切换到右侧摄像头"; }
        if (action == "record_on")  { state_.set("camera_recording", "true");  return "录像已开启"; }
        if (action == "record_off") { state_.set("camera_recording", "false"); return "录像已停止"; }
        return "摄像头指令已执行";
    }
};

