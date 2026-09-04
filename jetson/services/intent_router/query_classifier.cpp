#include "query_classifier.h"
#include "semantic_router.h"
#include <algorithm>
#include <regex>
#include <iostream>
#include <cmath>

namespace edge_llm_rag
{

    // ═══════════════════════════════════════════════════════════
    // 构造
    // ═══════════════════════════════════════════════════════════

    QueryClassifier::QueryClassifier()
    {
        initialize_keyword_dictionary();
    }

    // ═══════════════════════════════════════════════════════════
    // 主入口 — 双通路融合分类
    // ═══════════════════════════════════════════════════════════

    QueryClassification QueryClassifier::classify_query(const std::string &query)
    {
        // 通路 A: Semantic Router (主)
        QueryClassification semantic_cls = classify_by_semantic(query);

        // 通路 B: 规则引擎 (兜底 + 紧急直通保护)
        QueryFeatures features = analyze_query_features(query);
        QueryClassification rule_cls = classify_by_rules(query, features);

        // 融合
        QueryClassification final_cls = merge_classifications(semantic_cls, rule_cls);

        // 计算最终置信度
        final_cls.confidence = std::max(semantic_cls.confidence,
            (rule_cls.query_type == QueryClassification::EMERGENCY_QUERY) ? 1.0f
            : (rule_cls.query_type == QueryClassification::EXPLICIT_COMMAND) ? 0.9f
            : 0.0f);

        return final_cls;
    }

    // ═══════════════════════════════════════════════════════════
    // 通路 B: 规则引擎 (保留 v1 的全部逻辑 + 新增 EXPLICIT_COMMAND)
    // ═══════════════════════════════════════════════════════════

    QueryFeatures QueryClassifier::analyze_query_features(const std::string &query)
    {
        QueryFeatures features;
        features.query_length = static_cast<int>(query.length());

        features.keywords = extract_keywords(query);

        features.urgency_score   = calculate_urgency_score(features.keywords);
        features.complexity_score = calculate_complexity_score(query, features.keywords);
        features.factual_score   = calculate_factual_score(features.keywords);
        features.creative_score  = calculate_creative_score(features.keywords);
        features.command_score   = calculate_command_score(features.keywords);   // 新增

        features.contains_question_words  = detect_question_words(query);
        features.contains_emergency_words = detect_emergency_words(query);
        features.contains_technical_words = detect_technical_words(query);
        features.contains_command_words   = detect_command_words(query);          // 新增

        return features;
    }

    QueryClassification QueryClassifier::classify_by_rules(const std::string &query,
                                                           const QueryFeatures &features)
    {
        QueryClassification cls;
        cls.reasoning = "";

        // ── 优先级 1: 紧急查询 (不可被覆盖) ─────────────────
        if (features.urgency_score > 0.7f || features.contains_emergency_words)
        {
            cls.query_type = QueryClassification::EMERGENCY_QUERY;
            cls.requires_immediate_response = true;
            cls.needs_rag_context = true;
            cls.needs_llm          = false;
            cls.allows_tool_call   = false;
            cls.reasoning = "规则: 紧急词命中";
            return cls;
        }

        // ── 优先级 2: 明确指令 (工具直达, 不经过LLM) ────────
        if (features.command_score > 0.6f || features.contains_command_words)
        {
            cls.query_type = QueryClassification::EXPLICIT_COMMAND;
            cls.requires_immediate_response = true;
            cls.needs_rag_context = false;
            cls.needs_llm          = false;
            cls.allows_tool_call   = false;
            cls.reasoning = "规则: 指令词命中";
            return cls;
        }

        // ── 优先级 2.5: 实时车况查询（工具域）──────────────
        // 车速/油量/电量/续航只有工具能答（手册无"当前值"）；必须走 LLM(agent)
        // 选工具而非 RAG-only。优先级高于语义通路：E1 实测"查看胎压和车速"
        // 被语义判 FACTUAL 走 RAG-only 后，车速部分无人应答（回归 case4）。
        for (const char *w : {"车速", "油量", "电量", "续航", "传感器"})
            if (query.find(w) != std::string::npos)
            {
                cls.query_type = QueryClassification::EXPLICIT_COMMAND;
                cls.requires_immediate_response = true;
                cls.needs_rag_context = false;
                cls.needs_llm          = true;
                cls.allows_tool_call   = true;
                cls.reasoning = "规则: 实时车况词命中（工具域）";
                return cls;
            }

        // ── 优先级 3-6: 原有逻辑 ─────────────────────────────
        if (features.factual_score >= 0.5f)
        {
            cls.query_type = QueryClassification::FACTUAL_QUERY;
            cls.needs_rag_context = true;
            cls.needs_llm          = false;
            cls.allows_tool_call   = false;
            cls.reasoning = "规则: factual >= 0.5";
        }
        else if (features.creative_score > 0.6f)
        {
            cls.query_type = QueryClassification::CREATIVE_QUERY;
            cls.needs_rag_context = false;
            cls.needs_llm          = true;
            cls.allows_tool_call   = true;
            cls.reasoning = "规则: creative > 0.6";
        }
        else if (features.complexity_score > 0.6f)
        {
            cls.query_type = QueryClassification::COMPLEX_QUERY;
            cls.needs_rag_context = true;
            cls.needs_llm          = true;
            cls.allows_tool_call   = true;
            cls.reasoning = "规则: complexity > 0.6";
        }
        else
        {
            cls.query_type = QueryClassification::UNKNOWN_QUERY;
            cls.needs_rag_context = true;
            cls.needs_llm          = true;
            cls.allows_tool_call   = true;
            cls.reasoning = "规则: 兜底 → UNKNOWN";
        }

        cls.requires_immediate_response =
            (cls.query_type == QueryClassification::EMERGENCY_QUERY);
        return cls;
    }

    // ═══════════════════════════════════════════════════════════
    // 通路 A: Semantic Router（E1 真实现：query 编码 → 意图中心余弦 → 类型）
    // ═══════════════════════════════════════════════════════════

    void QueryClassifier::attach_semantic(
        SemanticRouter *router,
        std::function<std::vector<float>(const std::string &)> encode)
    {
        semantic_router_ = router;
        encode_fn_       = std::move(encode);
    }

    QueryClassification QueryClassifier::classify_by_semantic(const std::string &query)
    {
        QueryClassification cls;

        if (!semantic_enabled_ || semantic_router_ == nullptr || !encode_fn_)
        {
            cls.reasoning = "Semantic: 未接线（无意图中心或未注入编码函数）";
            return cls;
        }

        std::vector<float> embedding;
        try
        {
            embedding = encode_fn_(query);
        }
        catch (...)
        {
            embedding.clear();
        }
        if (embedding.empty())
        {
            cls.reasoning = "Semantic: encode 失败（RAG embed 端点无应答）";
            return cls;
        }

        SemanticResult result = semantic_router_->route(embedding);
        last_semantic_confidence_ = result.confidence;

        static const std::unordered_map<std::string, QueryClassification::QueryType> kNameToType = {
            {"EMERGENCY",    QueryClassification::EMERGENCY_QUERY},
            {"FACTUAL",      QueryClassification::FACTUAL_QUERY},
            {"EXPLICIT_CMD", QueryClassification::EXPLICIT_COMMAND},
            {"COMPLEX",      QueryClassification::COMPLEX_QUERY},
            {"CREATIVE",     QueryClassification::CREATIVE_QUERY},
        };
        auto it = kNameToType.find(result.intent_name);
        if (!result.matched || it == kNameToType.end())
        {
            cls.reasoning = "Semantic: top1=" + result.intent_name +
                            " conf=" + std::to_string(result.confidence).substr(0, 4) +
                            " 未过阈值";
            return cls;
        }

        cls.query_type = it->second;
        cls.confidence = result.confidence;
        switch (cls.query_type)
        {
        case QueryClassification::EMERGENCY_QUERY:
            cls.requires_immediate_response = true;
            cls.needs_rag_context = true;
            break;
        case QueryClassification::FACTUAL_QUERY:
            cls.needs_rag_context = true;
            break;
        case QueryClassification::EXPLICIT_COMMAND:
            cls.requires_immediate_response = true;
            cls.needs_llm         = true;   // 指令走 LLM(agent) 选工具，不经 RAG
            cls.allows_tool_call  = true;
            break;
        case QueryClassification::COMPLEX_QUERY:
            cls.needs_rag_context = true;
            cls.needs_llm         = true;
            cls.allows_tool_call  = true;
            break;
        case QueryClassification::CREATIVE_QUERY:
            cls.needs_llm         = true;
            cls.allows_tool_call  = true;
            break;
        default:
            break;
        }
        cls.reasoning = "Semantic: " + result.intent_name +
                        " conf=" + std::to_string(result.confidence).substr(0, 4);
        return cls;
    }

    // ═══════════════════════════════════════════════════════════
    // 融合决策
    // ═══════════════════════════════════════════════════════════

    QueryClassification QueryClassifier::merge_classifications(
        const QueryClassification &semantic,
        const QueryClassification &rule)
    {
        // 规则 1: 紧急查询不可被覆盖
        if (rule.query_type == QueryClassification::EMERGENCY_QUERY)
            return rule;

        // 规则 2: 明确指令 (规则引擎) 优先
        if (rule.query_type == QueryClassification::EXPLICIT_COMMAND)
            return rule;

        // 规则 3: Semantic 高置信度 (>= 0.7) → 采纳 semantic
        if (semantic.confidence >= 0.7f &&
            semantic.query_type != QueryClassification::UNKNOWN_QUERY)
            return semantic;

        // 规则 3.5: 规则通路无证据（UNKNOWN=关键词全 miss）而语义已过自身阈值
        // → 采纳 semantic。改述句（"多久做一次首保"）规则只能给 UNKNOWN，
        // 此时语义是唯一有效信号；实测 conf≈0.58 的正确 FACTUAL 曾被 0.6
        // 投票门槛压回 UNKNOWN 走 LLM 慢路径（9.4s），此规则修复该类回归。
        if (rule.query_type == QueryClassification::UNKNOWN_QUERY &&
            semantic.query_type != QueryClassification::UNKNOWN_QUERY)
            return semantic;

        // 规则 4: Semantic 中等置信度 (0.4~0.7) → 与规则投票
        if (semantic.confidence >= 0.4f &&
            semantic.query_type != QueryClassification::UNKNOWN_QUERY)
        {
            if (semantic.query_type == rule.query_type)
                return semantic;   // 一致, 取 semantic

            // 不一致, 取置信度更高的一方
            return (semantic.confidence > 0.6f) ? semantic : rule;
        }

        // 规则 5: Semantic 低置信度 → 退回规则引擎
        return rule;
    }

    // ═══════════════════════════════════════════════════════════
    // 路由配置构建
    // ═══════════════════════════════════════════════════════════

    RouteConfig QueryClassifier::build_route_config(
        const QueryClassification &cls)
    {
        RouteConfig cfg{};

        switch (cls.query_type)
        {
        case QueryClassification::EMERGENCY_QUERY:
            cfg = {true,  false, false, false};  // RAG → TTS (快路径)
            break;

        case QueryClassification::FACTUAL_QUERY:
            cfg = {true,  false, false, false};  // RAG → TTS (快路径)
            break;

        case QueryClassification::EXPLICIT_COMMAND:
            cfg = {false, false, true,  false};  // Tool → TTS (快路径, 绕过大模型)
            break;

        case QueryClassification::COMPLEX_QUERY:
            cfg = {true,  true,  false, true};   // RAG → LLM → TTS (慢路径, 可回调Tool)
            break;

        case QueryClassification::CREATIVE_QUERY:
            cfg = {false, true,  false, true};   // LLM → TTS (慢路径, 可回调Tool)
            break;

        case QueryClassification::UNKNOWN_QUERY:
        default:
            cfg = {true,  true,  false, true};   // 兜底: RAG → LLM → TTS
            break;
        }

        return cfg;
    }

    // ═══════════════════════════════════════════════════════════
    // Semantic Router 开关
    // ═══════════════════════════════════════════════════════════

    void QueryClassifier::set_semantic_enabled(bool enabled)
    {
        semantic_enabled_ = enabled;
    }

    float QueryClassifier::get_last_semantic_confidence() const
    {
        return last_semantic_confidence_;
    }

    // ═══════════════════════════════════════════════════════════
    // 关键词词典初始化 (v2: 新增 command 词典)
    // ═══════════════════════════════════════════════════════════

    void QueryClassifier::initialize_keyword_dictionary()
    {
        // ── v1 保留 ──────────────────────────────────────────
        keyword_dict_["emergency"] = {
            "故障", "警告", "危险", "紧急", "异常", "失灵", "失效", "损坏",
            "发动机故障", "制动故障", "转向故障", "电气故障", "安全气囊", "ABS故障"};

        keyword_dict_["technical"] = {
            "发动机", "制动", "变速箱", "电气", "空调", "转向", "悬挂", "轮胎",
            "机油", "冷却液", "制动液", "变速箱油", "电瓶", "发电机", "起动机"};

        keyword_dict_["maintenance"] = {
            "保养", "维修", "更换", "检查", "清洁", "调整", "润滑", "紧固",
            "定期保养", "机油更换", "滤清器", "火花塞", "制动片", "轮胎更换"};

        keyword_dict_["feature"] = {
            "自动泊车", "车道保持", "定速巡航", "导航", "娱乐", "空调控制",
            "座椅调节", "后视镜", "雨刷", "灯光", "音响", "蓝牙"};

        keyword_dict_["question"] = {
            "什么", "怎么", "如何", "为什么", "哪里", "何时", "多少", "哪个",
            "吗", "呢", "嘛", "能不能", "可不可以", "有没有",
            "推荐一下", "怎么去", "去哪里", "怎么玩"};

        keyword_dict_["creative"] = {
            "推荐", "建议", "想法", "创意", "优化", "改进", "设计", "规划",
            "旅游", "旅行", "景点", "门票", "酒店", "美食",
            "天气", "笑话", "故事", "新闻", "百科", "科普",
            "今天", "明天", "现在", "附近", "哪里有", "怎么走"};

        // ── v2 新增: 明确指令词 ──────────────────────────────
        keyword_dict_["command"] = {
            "打开", "关闭", "开启", "关上", "调高", "调低", "调到",
            "设置", "切换", "启动", "停止", "暂停", "继续", "取消",
            "播放", "下一首", "上一首", "静音", "音量", "接听", "挂断",
            "导航到", "打电话给", "发消息", "拍照", "录像",
            "空调", "车窗", "天窗", "座椅加热", "方向盘加热",
            "后视镜加热", "除霜", "内循环", "外循环"};
    }

    // ═══════════════════════════════════════════════════════════
    // 提取关键词 (不变)
    // ═══════════════════════════════════════════════════════════

    std::vector<std::string> QueryClassifier::extract_keywords(const std::string &query)
    {
        std::vector<std::string> keywords;
        for (const auto &[category, words] : keyword_dict_)
        {
            for (const auto &word : words)
            {
                if (query.find(word) != std::string::npos)
                {
                    keywords.push_back(word);
                }
            }
        }
        return keywords;
    }

    // ═══════════════════════════════════════════════════════════
    // v1 打分函数 (保留)
    // ═══════════════════════════════════════════════════════════

    float QueryClassifier::calculate_urgency_score(
        const std::vector<std::string> &keywords)
    {
        int count = 0;
        for (const auto &kw : keywords)
        {
            if (std::find(keyword_dict_["emergency"].begin(),
                          keyword_dict_["emergency"].end(), kw)
                != keyword_dict_["emergency"].end())
                count++;
        }
        return std::min(1.0f, static_cast<float>(count) * 0.3f);
    }

    float QueryClassifier::calculate_complexity_score(
        const std::string &query,
        const std::vector<std::string> &keywords)
    {
        float score = 0.0f;
        score += std::min(1.0f, static_cast<float>(query.length()) / 100.0f) * 0.3f;
        score += std::min(1.0f, static_cast<float>(keywords.size()) / 10.0f) * 0.4f;

        int technical_count = 0;
        for (const auto &kw : keywords)
        {
            if (std::find(keyword_dict_["technical"].begin(),
                          keyword_dict_["technical"].end(), kw)
                != keyword_dict_["technical"].end())
                technical_count++;
        }
        score += std::min(1.0f, static_cast<float>(technical_count) / 5.0f) * 0.3f;

        return std::min(1.0f, score);
    }

    float QueryClassifier::calculate_factual_score(
        const std::vector<std::string> &keywords)
    {
        float score = 0.0f;
        for (const auto &kw : keywords)
        {
            if (std::find(keyword_dict_["technical"].begin(),
                          keyword_dict_["technical"].end(), kw)
                != keyword_dict_["technical"].end())
                score += 0.4f;
            if (std::find(keyword_dict_["maintenance"].begin(),
                          keyword_dict_["maintenance"].end(), kw)
                != keyword_dict_["maintenance"].end())
                score += 0.4f;
            if (std::find(keyword_dict_["feature"].begin(),
                          keyword_dict_["feature"].end(), kw)
                != keyword_dict_["feature"].end())
                score += 0.5f;
        }
        return std::min(1.0f, score);
    }

    float QueryClassifier::calculate_creative_score(
        const std::vector<std::string> &keywords)
    {
        float score = 0.0f;
        for (const auto &kw : keywords)
        {
            if (std::find(keyword_dict_["creative"].begin(),
                          keyword_dict_["creative"].end(), kw)
                != keyword_dict_["creative"].end())
                score += 0.3f;
        }
        return std::min(1.0f, score);
    }

    // ── v2 新增: 指令性得分 ─────────────────────────────────
    float QueryClassifier::calculate_command_score(
        const std::vector<std::string> &keywords)
    {
        float score = 0.0f;
        for (const auto &kw : keywords)
        {
            if (std::find(keyword_dict_["command"].begin(),
                          keyword_dict_["command"].end(), kw)
                != keyword_dict_["command"].end())
                score += 0.35f;
        }
        return std::min(1.0f, score);
    }

    // ═══════════════════════════════════════════════════════════
    // 检测函数
    // ═══════════════════════════════════════════════════════════

    bool QueryClassifier::detect_question_words(const std::string &query)
    {
        for (const auto &w : keyword_dict_["question"])
            if (query.find(w) != std::string::npos) return true;
        return false;
    }

    bool QueryClassifier::detect_emergency_words(const std::string &query)
    {
        for (const auto &w : keyword_dict_["emergency"])
            if (query.find(w) != std::string::npos) return true;
        return false;
    }

    bool QueryClassifier::detect_technical_words(const std::string &query)
    {
        for (const auto &w : keyword_dict_["technical"])
            if (query.find(w) != std::string::npos) return true;
        return false;
    }

    // ── v2 新增 ──────────────────────────────────────────────
    bool QueryClassifier::detect_command_words(const std::string &query)
    {
        for (const auto &w : keyword_dict_["command"])
            if (query.find(w) != std::string::npos) return true;
        return false;
    }

    // ═══════════════════════════════════════════════════════════
    // 领域判断 (保留)
    // ═══════════════════════════════════════════════════════════

    std::string QueryClassifier::determine_domain(const std::string &query,
                                                   const std::vector<std::string> &keywords)
    {
        for (const auto &kw : keywords)
        {
            if (std::find(keyword_dict_["emergency"].begin(),
                          keyword_dict_["emergency"].end(), kw)
                != keyword_dict_["emergency"].end())
                return "emergency";
            if (std::find(keyword_dict_["technical"].begin(),
                          keyword_dict_["technical"].end(), kw)
                != keyword_dict_["technical"].end())
                return "technical";
            if (std::find(keyword_dict_["maintenance"].begin(),
                          keyword_dict_["maintenance"].end(), kw)
                != keyword_dict_["maintenance"].end())
                return "maintenance";
        }
        return "general";
    }

} // namespace edge_llm_rag
