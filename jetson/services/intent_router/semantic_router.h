#pragma once

#include <string>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <cstring>

// ═══════════════════════════════════════════════════════════════
// 轻量级语义路由器 — 纯 C++ 实现，无 Python 依赖
//
// 核心思路:
//   预置每个意图的示例句 → 用 sentence-transformers 离线编码为 N×768 矩阵
//   → 启动时加载到内存
//   → 运行时: query_embed · intent_centers → cosine → top-1
//
// 依赖: 仅需调用 Python 侧的 encode() (已有 pybind11)
//       或直接用 ONNX Runtime 在 C++ 侧推理
// ═══════════════════════════════════════════════════════════════

namespace edge_llm_rag
{

    // ── 一个意图的定义 ──────────────────────────────────────
    struct IntentRoute
    {
        std::string              name;
        std::vector<float>       center;       // 该意图的中心向量 (768维)
        float                    threshold;    // 最低匹配阈值
        int                      priority;     // 优先级 (越小越高, 用于冲突解决)
    };

    // ── 语义路由结果 ────────────────────────────────────────
    struct SemanticResult
    {
        std::string intent_name;
        float       confidence;
        bool        matched;                  // confidence >= threshold
    };

    // ── 语义路由器 ──────────────────────────────────────────
    class SemanticRouter
    {
    public:
        SemanticRouter();

        /// 注册一个意图
        void add_intent(const std::string &name,
                        const std::vector<float> &center,
                        float threshold = 0.4f,
                        int   priority  = 10);

        /// 核心: 查询 → 意图匹配
        SemanticResult route(const std::vector<float> &query_embedding) const;

        /// 批量设置意图中心 (从文件加载)
        void load_intents(const std::vector<IntentRoute> &routes);

        /// 是否启用
        void set_enabled(bool enabled) { enabled_ = enabled; }
        bool is_enabled() const { return enabled_; }

        /// 获取已注册意图数
        size_t intent_count() const { return intents_.size(); }

        /// 只读访问意图表（启动日志/调试用）
        const std::vector<IntentRoute> &intents_view() const { return intents_; }

    /// 从二进制文件加载意图中心 (build_intent_centers.py 产出)
    /// 文件格式: [int32 num][int32 dim] for each: [int32 name_len][char* name][int32 priority][float thr][float*dim center]
    bool load_from_file(const std::string &path);

    /// 意图中心向量维度（未加载=0）
    int dim() const { return dim_; }

    private:
    std::vector<IntentRoute> intents_;
    int                      dim_    = 0;
    bool                     enabled_ = true;

        /// 计算两个向量的余弦相似度
        static float cosine_similarity(const std::vector<float> &a,
                                       const std::vector<float> &b);
    };

    // ── 内联实现 ────────────────────────────────────────────

    inline SemanticRouter::SemanticRouter() = default;

    inline void SemanticRouter::add_intent(const std::string &name,
                                           const std::vector<float> &center,
                                           float threshold,
                                           int   priority)
    {
        intents_.push_back({name, center, threshold, priority});
        // 按优先级排序 (小值优先)
        std::sort(intents_.begin(), intents_.end(),
                  [](const IntentRoute &a, const IntentRoute &b) {
                      return a.priority < b.priority;
                  });
    }

    inline void SemanticRouter::load_intents(const std::vector<IntentRoute> &routes)
    {
        intents_ = routes;
        std::sort(intents_.begin(), intents_.end(),
                  [](const IntentRoute &a, const IntentRoute &b) {
                      return a.priority < b.priority;
                  });
    }

    inline SemanticResult SemanticRouter::route(
        const std::vector<float> &query_embedding) const
    {
        SemanticResult best;
        best.intent_name = "UNKNOWN";
        best.confidence  = 0.0f;
        best.matched     = false;

        if (!enabled_ || intents_.empty())
            return best;

        // 遍历所有意图, 找余弦相似度最高的
        for (const auto &intent : intents_)
        {
            float sim = cosine_similarity(query_embedding, intent.center);
            if (sim > best.confidence)
            {
                best.confidence  = sim;
                best.intent_name = intent.name;
                best.matched     = (sim >= intent.threshold);
            }
        }

        return best;
    }

    inline float SemanticRouter::cosine_similarity(
        const std::vector<float> &a,
        const std::vector<float> &b)
    {
        if (a.size() != b.size() || a.empty())
            return 0.0f;

        float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
        {
            dot   += a[i] * b[i];
            norm_a += a[i] * a[i];
            norm_b += b[i] * b[i];
        }

        float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
        if (denom < 1e-10f)
            return 0.0f;

        return dot / denom;
    }

    inline bool SemanticRouter::load_from_file(const std::string &path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return false;

        int32_t num_intents = 0;
        file.read(reinterpret_cast<char *>(&num_intents), sizeof(int32_t));
        if (num_intents <= 0 || num_intents > 100)
            return false;

        int32_t dim = 0;
        file.read(reinterpret_cast<char *>(&dim), sizeof(int32_t));
        if (dim < 64 || dim > 1024)
            return false;

        intents_.clear();
        dim_ = dim;
        for (int i = 0; i < num_intents; ++i)
        {
            // 读取 name
            int32_t name_len = 0;
            file.read(reinterpret_cast<char *>(&name_len), sizeof(int32_t));
            if (name_len <= 0 || name_len > 64)
                return false;
            std::string name(name_len, '\0');
            file.read(name.data(), name_len);

            // 读取 priority, threshold
            int32_t priority = 0;
            float   threshold = 0.0f;
            file.read(reinterpret_cast<char *>(&priority),  sizeof(int32_t));
            file.read(reinterpret_cast<char *>(&threshold), sizeof(float));

            // 读取 center vector (dim floats)
            std::vector<float> center(static_cast<size_t>(dim));
            file.read(reinterpret_cast<char *>(center.data()),
                      static_cast<std::streamsize>(dim) * sizeof(float));

            if (!file.good())
                return false;

            intents_.push_back({std::move(name), std::move(center), threshold, priority});
        }

        // 按优先级排序
        std::sort(intents_.begin(), intents_.end(),
                  [](const IntentRoute &a, const IntentRoute &b) {
                      return a.priority < b.priority;
                  });

        return true;
    }

} // namespace edge_llm_rag
