// rknn_detector.cpp — NV12 → RGA → RKNN(NPU) → YOLOv5 后处理（R10）
#define VOX_LOG_TAG "rk.detect"

#include "rknn_detector.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <mutex>

#include "vox_log.h"

// im2d/rga 头自带 C++ 链接声明（含重载），不得包 extern "C"；rknn_api.h 是纯 C
#include <im2d.h>
#include <rga.h>
extern "C" {
#include "rknn_api.h"
}

namespace vox {
namespace {

// YOLOv5 anchors（三尺度各 3 组，与 prj2/Rockchip demo 一致）
constexpr int kAnchors[3][6] = {
    {10, 13, 16, 30, 33, 23}, {30, 61, 62, 45, 59, 119}, {116, 90, 156, 198, 373, 326}};

inline float deqnt(int8_t q, int32_t zp, float scale) {
  return (static_cast<float>(q) - static_cast<float>(zp)) * scale;
}
inline int8_t qnt(float f, int32_t zp, float scale) {
  float v = f / scale + zp;
  return static_cast<int8_t>(v < -128 ? -128 : (v > 127 ? 127 : v));
}

// 单尺度解码：通道序 [3*(5+C)][grid_h][grid_w]（int8 量化输出，prj2 同板同模型已验证）
int decode_scale(const int8_t* input, const int* anchor, int grid_h, int grid_w,
                 int model_h, int model_w, int stride, int class_num,
                 const std::set<int>& targets, float box_thresh, int32_t zp,
                 float scale, std::vector<float>& boxes, std::vector<float>& probs,
                 std::vector<int>& cls_ids) {
  const int grid_len = grid_h * grid_w;
  const int8_t thres_i8 = qnt(box_thresh, zp, scale);
  int valid = 0;
  for (int a = 0; a < 3; ++a) {
    for (int i = 0; i < grid_h; ++i) {
      for (int j = 0; j < grid_w; ++j) {
        const int8_t box_conf = input[((5 + class_num) * a + 4) * grid_len + i * grid_w + j];
        if (box_conf < thres_i8) continue;
        const int8_t* p = input + ((5 + class_num) * a) * grid_len + i * grid_w + j;
        int max_cls = 0;
        int8_t max_prob = p[5 * grid_len];
        for (int k = 1; k < class_num; ++k) {
          const int8_t v = p[(5 + k) * grid_len];
          if (v > max_prob) {
            max_prob = v;
            max_cls = k;
          }
        }
        if (max_prob <= thres_i8) continue;
        if (!targets.empty() && targets.count(max_cls) == 0) continue;  // 类别白名单
        float x = (deqnt(*p, zp, scale)) * 2.0f - 0.5f;
        float y = (deqnt(p[grid_len], zp, scale)) * 2.0f - 0.5f;
        float w = (deqnt(p[2 * grid_len], zp, scale)) * 2.0f;
        float h = (deqnt(p[3 * grid_len], zp, scale)) * 2.0f;
        x = (x + j) * stride;
        y = (y + i) * stride;
        w = w * w * anchor[a * 2];
        h = h * h * anchor[a * 2 + 1];
        boxes.push_back(x - w / 2);
        boxes.push_back(y - h / 2);
        boxes.push_back(w);
        boxes.push_back(h);
        probs.push_back(deqnt(max_prob, zp, scale) * deqnt(box_conf, zp, scale));
        cls_ids.push_back(max_cls);
        ++valid;
      }
    }
  }
  (void)model_h;
  (void)model_w;
  return valid;
}

float iou(const float* a, const float* b) {
  const float ix0 = std::max(a[0], b[0]), iy0 = std::max(a[1], b[1]);
  const float ix1 = std::min(a[0] + a[2], b[0] + b[2]);
  const float iy1 = std::min(a[1] + a[3], b[1] + b[3]);
  const float iw = std::max(0.f, ix1 - ix0), ih = std::max(0.f, iy1 - iy0);
  const float inter = iw * ih;
  const float uni = a[2] * a[3] + b[2] * b[3] - inter;
  return uni <= 0.f ? 0.f : inter / uni;
}

}  // namespace

RknnDetector::~RknnDetector() {
  if (ctx_) rknn_destroy(reinterpret_cast<rknn_context>(ctx_));
  if (model_data_) free(model_data_);
}

bool RknnDetector::start(const Params& p) {
  params_ = p;
  // 读模型文件
  FILE* fp = fopen(p.model_path.c_str(), "rb");
  if (!fp) {
    error_ = "打开模型失败: " + p.model_path;
    VOX_ERROR("%s", error_.c_str());
    return false;
  }
  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  model_data_ = malloc(size);
  if (!model_data_ || fread(model_data_, 1, size, fp) != static_cast<size_t>(size)) {
    error_ = "读取模型失败";
    fclose(fp);
    return false;
  }
  fclose(fp);

  rknn_context ctx = 0;
  int ret = rknn_init(&ctx, model_data_, size, 0, nullptr);
  if (ret < 0) {
    error_ = "rknn_init rc=" + std::to_string(ret);
    VOX_ERROR("%s", error_.c_str());
    return false;
  }
  ctx_ = reinterpret_cast<void*>(ctx);
  rknn_set_core_mask(ctx, RKNN_NPU_CORE_0);  // 单模型单核，留双核余量

  rknn_sdk_version ver{};
  rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver));
  rknn_input_output_num io{};
  rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
  n_output_ = io.n_output;

  rknn_tensor_attr in_attr{};
  in_attr.index = 0;
  rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
  if (in_attr.fmt == RKNN_TENSOR_NCHW) {
    model_ch_ = in_attr.dims[1];
    model_h_ = in_attr.dims[2];
    model_w_ = in_attr.dims[3];
  } else {
    model_h_ = in_attr.dims[1];
    model_w_ = in_attr.dims[2];
    model_ch_ = in_attr.dims[3];
  }

  // 输出属性：量化参数 + 网格尺寸（按 stride 排到 8/16/32 槽位），class_num 自动推导
  out_scales_.assign(3, 0.f);
  out_zps_.assign(3, 0);
  out_grid_h_.assign(3, 0);
  for (int i = 0; i < n_output_ && i < 3; ++i) {
    rknn_tensor_attr attr{};
    attr.index = i;
    rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
    // 布局 [3*(5+C)][H][W] 或带 batch 维 [1][3*(5+C)][H][W]（本板模型实测后者）；
    // dims[0]==1 视为 NCHW 批次形式，取 dims[1]=通道、dims[2]=grid_h
    const int ch = (attr.dims[0] == 1) ? attr.dims[1] : attr.dims[0];
    const int grid_h = (attr.dims[0] == 1) ? attr.dims[2] : attr.dims[1];
    if (ch <= 0 || ch % 3 != 0 || model_h_ % grid_h != 0) {
      error_ = "输出布局与预期不符（dims0=" + std::to_string(attr.dims[0]) +
               " dims1=" + std::to_string(attr.dims[1]) + "）";
      VOX_ERROR("%s", error_.c_str());
      return false;
    }
    const int stride = model_h_ / grid_h;
    const int slot = stride == 8 ? 0 : stride == 16 ? 1 : stride == 32 ? 2 : -1;
    if (slot < 0) {
      error_ = "未知 stride=" + std::to_string(stride);
      VOX_ERROR("%s", error_.c_str());
      return false;
    }
    out_scales_[slot] = attr.scale;
    out_zps_[slot] = attr.zp;
    out_grid_h_[slot] = grid_h;
    const int cn = ch / 3 - 5;
    if (class_num_ == 0) class_num_ = cn;
    if (class_num_ != cn) {
      error_ = "三输出 class_num 不一致";
      VOX_ERROR("%s", error_.c_str());
      return false;
    }
  }

  nv12_scaled_.resize(static_cast<size_t>(model_w_) * model_h_ * 3 / 2);
  rgb_.resize(static_cast<size_t>(model_w_) * model_h_ * 3);
  VOX_INFO("detector ready: %s (%dx%d C=%d) NPU core0, %zu 目标类别, sdk %s",
           p.model_path.c_str(), model_w_, model_h_, class_num_, p.target_classes.size(),
           ver.api_version);
  return true;
}

std::vector<Detection> RknnDetector::detect_nv12(const uint8_t* nv12, int src_w, int src_h) {
  std::vector<Detection> out;
  if (!ctx_ || !nv12) return out;
  const auto t0 = std::chrono::steady_clock::now();

  // RGA 两步（librga 2.x rga_buffer_t API）：NV12 缩放到模型尺寸 → 转 RGB888
  rga_buffer_t rga_src = wrapbuffer_virtualaddr(
      const_cast<uint8_t*>(nv12), src_w, src_h, RK_FORMAT_YCbCr_420_SP);
  rga_buffer_t rga_mid = wrapbuffer_virtualaddr(
      nv12_scaled_.data(), model_w_, model_h_, RK_FORMAT_YCbCr_420_SP);
  rga_buffer_t rga_rgb =
      wrapbuffer_virtualaddr(rgb_.data(), model_w_, model_h_, RK_FORMAT_RGB_888);
  IM_STATUS s1 = imresize(rga_src, rga_mid);
  IM_STATUS s2 = imcvtcolor(rga_mid, rga_rgb, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888);
  // 本枚举 NOERROR=2 / SUCCESS=1 / 失败≤0（FAILED=0，负数为各类错误）
  const bool rga_ok = s1 > 0 && s2 > 0;
  if (!rga_ok) {
    VOX_ERROR("RGA 失败: resize=%d cvt=%d", static_cast<int>(s1), static_cast<int>(s2));
    return out;
  }

  rknn_context ctx = reinterpret_cast<rknn_context>(ctx_);
  rknn_input inputs[1]{};
  inputs[0].index = 0;
  inputs[0].type = RKNN_TENSOR_UINT8;
  inputs[0].fmt = RKNN_TENSOR_NHWC;
  inputs[0].size = rgb_.size();
  inputs[0].buf = rgb_.data();
  inputs[0].pass_through = 0;
  rknn_inputs_set(ctx, 1, inputs);

  std::vector<rknn_output> outputs(n_output_);
  for (auto& o : outputs) {
    o.want_float = 0;
    o.is_prealloc = 0;
  }
  if (rknn_run(ctx, nullptr) < 0 || rknn_outputs_get(ctx, n_output_, outputs.data(), nullptr) < 0) {
    VOX_ERROR("rknn run/outputs_get 失败");
    return out;
  }
  out_bufs_.resize(n_output_);
  for (int i = 0; i < n_output_; ++i) out_bufs_[i] = outputs[i].buf;

  out = postprocess(src_w, src_h);

  rknn_outputs_release(ctx, n_output_, outputs.data());
  out_bufs_.clear();

  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                        .count();
  stats_.inferences++;
  stats_.detections += out.size();
  stats_.infer_ms_ewma = stats_.infer_ms_ewma == 0.0 ? ms : stats_.infer_ms_ewma * 0.9 + ms * 0.1;
  stats_.infer_ms_max = std::max(stats_.infer_ms_max, ms);
  return out;
}

std::vector<Detection> RknnDetector::postprocess(int src_w, int src_h) {
  std::vector<Detection> out;
  std::vector<float> boxes, probs;
  std::vector<int> cls_ids;
  for (int slot = 0; slot < 3; ++slot) {
    if (out_grid_h_[slot] == 0 || slot >= static_cast<int>(out_bufs_.size())) continue;
    const int stride = (slot == 0) ? 8 : (slot == 1) ? 16 : 32;
    decode_scale(static_cast<const int8_t*>(out_bufs_[slot]), kAnchors[slot],
                 out_grid_h_[slot], out_grid_h_[slot], model_h_, model_w_, stride,
                 class_num_, params_.target_classes, params_.box_thresh, out_zps_[slot],
                 out_scales_[slot], boxes, probs, cls_ids);
  }
  const int n = static_cast<int>(probs.size());
  if (n == 0) return out;

  // 按置信度降序（索引排序，boxes/probs 不动）
  std::vector<int> order(n);
  for (int i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](int a, int b) { return probs[a] > probs[b]; });

  // 按类 NMS（贪心：保留高置信框，剔除同类 IoU>阈值的框）
  std::vector<bool> suppressed(n, false);
  const float scale_w = static_cast<float>(model_w_) / src_w;
  const float scale_h = static_cast<float>(model_h_) / src_h;
  for (int i = 0; i < n && static_cast<int>(out.size()) < 32; ++i) {
    const int a = order[i];
    if (suppressed[a]) continue;
    for (int j = i + 1; j < n; ++j) {
      const int b = order[j];
      if (!suppressed[b] && cls_ids[a] == cls_ids[b] &&
          iou(&boxes[a * 4], &boxes[b * 4]) > params_.nms_thresh) {
        suppressed[b] = true;
      }
    }
    Detection d;
    d.cls = cls_ids[a];
    d.prop = probs[a];
    d.left = static_cast<int>(boxes[a * 4] / scale_w);
    d.top = static_cast<int>(boxes[a * 4 + 1] / scale_h);
    d.right = static_cast<int>((boxes[a * 4] + boxes[a * 4 + 2]) / scale_w);
    d.bottom = static_cast<int>((boxes[a * 4 + 1] + boxes[a * 4 + 3]) / scale_h);
    d.left = std::max(0, std::min(d.left, src_w - 1));
    d.top = std::max(0, std::min(d.top, src_h - 1));
    d.right = std::max(d.left + 1, std::min(d.right, src_w));
    d.bottom = std::max(d.top + 1, std::min(d.bottom, src_h));
    out.push_back(d);
  }
  return out;
}

}  // namespace vox
