// rknn_detector.h — 轻量目标检测器（R10 事件锁录）
//
// 职责：NV12 紧凑帧 → RGA 缩放/转 RGB（640x640）→ RKNN INT8 推理（NPU 单核）
//       → YOLOv5 三输出后处理（类别过滤 + NMS），输出原始坐标系检测结果。
// 复用口径：后处理数学与 prj2（Rockchip Apache-2.0 demo）同源，修正其标签数组
//   越界与 labels[0] 写死两处；类别改为构造方注入的 ID 集合，class_num 从输出
//   张量维度自动推导（同一实现兼容 80 类与单类模型）。
// 线程契约：非线程安全，单检测线程独占使用（recorder_service 的检测线程）。
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace vox {

struct Detection {
  int cls = -1;        // 模型类别 ID（COCO：0=person 2=car 5=bus 7=truck）
  float prop = 0.0f;   // obj*cls 置信度
  int left = 0, top = 0, right = 0, bottom = 0;  // 原始输入坐标系
};

class RknnDetector {
 public:
  struct Params {
    std::string model_path;                  // .rknn 模型文件
    std::set<int> target_classes;            // 空=不过滤（全类别）
    float box_thresh = 0.25f;
    float nms_thresh = 0.45f;
  };

  struct Stats {
    uint64_t inferences = 0;      // 推理次数
    uint64_t detections = 0;      // 累计命中目标数（过滤后）
    double infer_ms_ewma = 0.0;   // 推理耗时（RGA+run+后处理）EWMA
    double infer_ms_max = 0.0;
  };

  ~RknnDetector();

  bool start(const Params& p);
  // nv12：紧凑 NV12（w×h，无行距）；返回原始坐标系的检测结果
  std::vector<Detection> detect_nv12(const uint8_t* nv12, int src_w, int src_h);

  bool running() const { return ctx_ != 0; }
  int model_width() const { return model_w_; }
  int model_height() const { return model_h_; }
  int class_num() const { return class_num_; }
  const Stats& stats() const { return stats_; }
  const std::string& error() const { return error_; }

 private:
  std::vector<Detection> postprocess(int src_w, int src_h);

  void* ctx_ = nullptr;            // rknn_context（void* 避免 header 泄漏到使用方）
  void* model_data_ = nullptr;
  // 输入/输出属性（rknn_tensor_attr 简化副本，构造时填充）
  int model_w_ = 0, model_h_ = 0, model_ch_ = 3;
  int class_num_ = 0;
  int n_output_ = 0;
  std::vector<float> out_scales_;
  std::vector<int32_t> out_zps_;
  std::vector<int> out_grid_h_;    // 各输出网格高（按 stride 8/16/32 排序后）
  std::vector<void*> out_bufs_;    // outputs_get 返回的原始缓冲（release 前有效）

  Params params_;
  Stats stats_;
  std::string error_;
  // RGA 中间缓冲：src NV12 → model 尺寸 NV12 → RGB888
  std::vector<uint8_t> nv12_scaled_;
  std::vector<uint8_t> rgb_;
};

}  // namespace vox
