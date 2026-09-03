#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace edge_llm_rag
{

    // ── 查询特征（保留原4维 + 新增指令维度）────────────────────
    struct QueryFeatures
    {
        std::vector<std::string> keywords;
        float urgency_score;            // 紧急性
        float complexity_score;         // 复杂度
        float factual_score;            // 事实性
        float creative_score;           // 创造性
        float command_score;            // ← 新增: 指令性 (是否可执行动作)
        int   query_length;
        bool  contains_question_words;
        bool  contains_emergency_words;
        bool  contains_technical_words;
        bool  contains_command_words;   // ← 新增: 是否包含动作词
    };

    // ── 查询分类（v2: 新增 EXPLICIT_COMMAND）──────────────────
    struct QueryClassification
    {
        enum QueryType
        {
            FACTUAL_QUERY,       // 事实查询    → RAG → TTS (快路径)
            EMERGENCY_QUERY,     // 紧急查询    → RAG → TTS (快路径)
            EXPLICIT_COMMAND,    // 明确指令    → Tool → TTS (快路径) ← 新增
            COMPLEX_QUERY,       // 复杂查询    → RAG → LLM → TTS
            CREATIVE_QUERY,      // 创意查询    → LLM → TTS
            UNKNOWN_QUERY        // 兜底        → RAG → LLM → TTS
        };

        QueryType   query_type;
        float       confidence;
        std::string reasoning;
        bool        requires_immediate_response;
        bool        needs_rag_context;     // ← 新增: 是否需要 RAG 注入
        bool        needs_llm;             // ← 新增: 是否需要 LLM
        bool        allows_tool_call;      // ← 新增: 是否允许 LLM 回调 Tool
    };

    // ── 路由配置（查询 → 路径）───────────────────────────────
    struct RouteConfig
    {
        bool use_rag;
        bool use_llm;
        bool use_tool_direct;   // 直接 Tool (不经过 LLM)
        bool allow_tool_loop;   // LLM 可回调 Tool
    };

    // ── 主分类器 ──────────────────────────────────────────────
    class QueryClassifier
    {
    public:
        explicit QueryClassifier();

        /// 完整分类流程
        QueryClassification classify_query(const std::string &query);

        /// 特征提取（公有，便于调试）
        QueryFeatures analyze_query_features(const std::string &query);

        /// 构建路由配置
        RouteConfig build_route_config(const QueryClassification &cls);

        // 关键词
        std::vector<std::string> extract_keywords(const std::string &query);
        std::string determine_domain(const std::string &query,
                                     const std::vector<std::string> &keywords);

        // ── 新增: Semantic Router 相关 ──────────────────────
        /// 设置是否启用语义路由 (默认 true)
        void set_semantic_enabled(bool enabled);
        /// 获取语义路由置信度 (用于调试)
        float get_last_semantic_confidence() const;

    private:
        std::unordered_map<std::string, std::vector<std::string>> keyword_dict_;

        void initialize_keyword_dictionary();

        // v1 打分函数（保留，作为规则兜底）
        float calculate_urgency_score(const std::vector<std::string> &keywords);
        float calculate_complexity_score(const std::string &query,
                                         const std::vector<std::string> &keywords);
        float calculate_factual_score(const std::vector<std::string> &keywords);
        float calculate_creative_score(const std::vector<std::string> &keywords);
        float calculate_command_score(const std::vector<std::string> &keywords);  // ← 新增

        // 检测函数
        bool detect_question_words(const std::string &query);
        bool detect_emergency_words(const std::string &query);
        bool detect_technical_words(const std::string &query);
        bool detect_command_words(const std::string &query);  // ← 新增

        // ── 新增: 语义路由 ──────────────────────────────────
        QueryClassification classify_by_semantic(const std::string &query);
        QueryClassification classify_by_rules(const QueryFeatures &features);
        QueryClassification merge_classifications(
            const QueryClassification &semantic,
            const QueryClassification &rule);

        bool   semantic_enabled_ = true;
        float  last_semantic_confidence_ = 0.0f;
    };

} // namespace edge_llm_rag
