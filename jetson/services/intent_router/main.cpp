// intent_router — 意图路由服务
//
// REP : port.intent_router（ASR 文本/查询进入）  PUB : port.intent_router_pub（全局状态总线）
// 下游：RAG :port.rag · LLM :port.llm · ToolBus :port.tool_bus · TTS :port.tts_text（异步线程）
//
// 协议（与 prj1 兼容）：
//   入：纯文本 query；出：命中路径下游服务的应答 JSON 原样透传
//   TTS 异步推送："<JSON转义文本> END"（TTS 以 " END" 为单条播报结束标记）
//
// 语义路由（E1 已接）：classify_query 双路融合——语义通路（意图中心余弦，编码经
// RAG embed 端点）+ 规则通路（关键词打分）兜底；意图中心文件由
// build_intent_centers.py 离线生成，conf router.intent_centers 指定，缺失则仅规则单路。
#define VOX_LOG_TAG "intent_router"
#include "../../common/vox_log.h"
#include "../../common/vox_config.h"
#include "../../common/json.hpp"

#include "query_classifier.h"
#include "semantic_router.h"
#include "ZmqClient.h"
#include "ZmqPub.h"
#include "ZmqServer.h"

#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <chrono>

namespace {

// ---------- JSON 小工具（统一 nlohmann，替换 prj1 手写字符串解析）----------

nlohmann::json jparse(const std::string& s) {
    return nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false);
}

std::string jstr(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

// 供 TTS "<文本> END" 协议的 JSON 字符串转义（中文正文通常无转义字符，行为与旧版一致）
std::string json_escape(const std::string& s) {
    std::string d = nlohmann::json(s).dump();
    return d.substr(1, d.size() - 2);
}

// ---------- 规则表：关键词 → 工具/动作/参数 ----------
// param 非空且 value 非空：填显式值（如 制热→heat）；
// param 非空且 value 为空：从 query 中提取参数词前紧邻数字（"调到22度"→temp=22），
// 提取不到则省略参数、由工具取自身默认值（替代 prj1 的 "default" 占位）。

struct Rule {
    const char* kw;
    const char* tool;
    const char* action;
    const char* param;   // "" = 无参数
    const char* value;   // "" = 数字提取
};

const Rule kRules[] = {
    // 行车记录（D2/R7：跨板真设备 dashcam 工具；LLM 未给出 tool_call 时的规则兜底，
    // 同前缀长词必须排在短词前（首条命中即返回））
    {"停止录像", "dashcam", "record_off",  "", ""},
    {"关闭录像", "dashcam", "record_off",  "", ""},
    {"暂停录像", "dashcam", "record_off",  "", ""},
    {"别录了",   "dashcam", "record_off",  "", ""},
    {"开始录像", "dashcam", "record_on",   "", ""},
    {"打开录像", "dashcam", "record_on",   "", ""},
    {"开启录像", "dashcam", "record_on",   "", ""},
    {"启动录像", "dashcam", "record_on",   "", ""},
    {"恢复录像", "dashcam", "record_on",   "", ""},
    {"拍张照",   "dashcam", "snapshot",    "", ""},
    {"拍照",     "dashcam", "snapshot",    "", ""},
    {"抓拍",     "dashcam", "snapshot",    "", ""},
    {"打开预览", "dashcam", "preview_on",  "", ""},
    {"开启预览", "dashcam", "preview_on",  "", ""},
    {"关闭预览", "dashcam", "preview_off", "", ""},
    {"停止预览", "dashcam", "preview_off", "", ""},
    {"预览",     "dashcam", "preview_on",  "", ""},
    {"录着",     "dashcam", "status",      "", ""},
    {"在录像",   "dashcam", "status",      "", ""},
    {"行车记录", "dashcam", "status",      "", ""},
    {"记录仪",   "dashcam", "status",      "", ""},
    {"存储",     "dashcam", "status",      "", ""},
    {"录像",     "dashcam", "status",      "", ""},
    // 空调
    {"调高温度", "climate_control", "set_temp", "temp", ""},
    {"调低温度", "climate_control", "set_temp", "temp", ""},
    {"调到",     "climate_control", "set_temp", "temp", ""},
    {"温度",     "climate_control", "set_temp", "temp", ""},
    {"空调",     "climate_control", "turn_on",   "",    ""},
    {"制冷",     "climate_control", "set_mode", "mode", "cool"},
    {"制热",     "climate_control", "set_mode", "mode", "heat"},
    {"通风",     "climate_control", "set_mode", "mode", "vent"},
    {"风量",     "climate_control", "set_fan",  "level", ""},
    {"风大",     "climate_control", "set_fan",  "level", "3"},
    {"风小",     "climate_control", "set_fan",  "level", "1"},
    {"内循环",   "climate_control", "set_mode", "mode", "recirculate"},
    {"外循环",   "climate_control", "set_mode", "mode", "outside"},
    // 车窗
    {"开车窗",   "window_control", "open_all",   "", ""},
    {"打开车窗", "window_control", "open_all",   "", ""},
    {"关车窗",   "window_control", "close_all",  "", ""},
    {"关闭车窗", "window_control", "close_all",  "", ""},
    {"车窗",     "window_control", "open_all",   "", ""},
    // 天窗
    {"天窗",     "sunroof_control", "open",      "", ""},
    {"开天窗",   "sunroof_control", "open",      "", ""},
    {"关天窗",   "sunroof_control", "close",     "", ""},
    // 座椅
    {"座椅加热", "seat_heater",     "on",        "level", ""},
    {"座椅通风", "seat_heater",     "off",       "",      ""},
    // 摄像头（多视角演示工具；"录像/拍照"已上移 dashcam 块——真设备优先）
    {"摄像头",   "camera_capture",  "front",     "", ""},
    {"前视",     "camera_capture",  "front",     "", ""},
    {"后视",     "camera_capture",  "rear",      "", ""},
    {"倒车影像", "camera_capture",  "rear",      "", ""},
    // 传感器
    {"传感器",    "sensor_read",    "read",      "", ""},
    {"胎压",      "sensor_read",    "read",      "", ""},
    {"车速",      "sensor_read",    "read",      "", ""},
    {"油量",      "sensor_read",    "read",      "", ""},
    {"里程",      "sensor_read",    "read",      "", ""},
    {"发动机温度", "sensor_read",   "read",      "", ""},
};

// 取 unit 前紧邻的数字串（"调到22度"→"22"）；无数字返回 ""
std::string number_before(const std::string& query, const std::string& unit) {
    size_t p = query.find(unit);
    if (p == std::string::npos || p == 0) return "";
    size_t s = p;
    while (s > 0 && query[s - 1] >= '0' && query[s - 1] <= '9') --s;
    return s < p ? query.substr(s, p - s) : "";
}

// 规则行 → 工具请求 JSON（dashcam 直通与 LLM 兜底共用）
std::string build_req_from_rule(const Rule& r, const std::string& query) {
    nlohmann::json req{{"tool", r.tool}, {"action", r.action}};
    if (r.param && *r.param) {
        if (r.value && *r.value) {
            req[r.param] = r.value;
        } else {
            // temp 按"度"、level 按"档"提取数字，其余无数字可提则省略
            std::string unit = std::strcmp(r.param, "temp") == 0 ? "度"
                               : std::strcmp(r.param, "level") == 0 ? "档" : "";
            if (!unit.empty()) {
                std::string v = number_before(query, unit);
                if (!v.empty()) req[r.param] = v;
            }
        }
    }
    return req.dump();
}

// 规则兜底：LLM 未给出 tool_call 细节时按关键词直接构造工具请求
std::string build_tool_request(const std::string& query) {
    for (const Rule& r : kRules) {
        if (query.find(r.kw) == std::string::npos) continue;
        return build_req_from_rule(r, query);
    }
    return "{\"tool\":\"sensor_read\",\"action\":\"read\"}";  // 兜底读传感器（保持 prj1 行为）
}

// 首个命中的规则（kRules 按域分块，dashcam 块在表头；返回 nullptr = 无命中）
const Rule* match_first_rule(const std::string& query) {
    for (const Rule& r : kRules)
        if (query.find(r.kw) != std::string::npos) return &r;
    return nullptr;
}

// ---------- 语音导航（UI v2.0：dashboard 多页 + 插件 nav_words，设计规范 7.3）----------
// conf dashboard.nav_words = "行车记录:dashcam 车控:vehicle …"（空格分隔 词:页面id）。
// 触发词（打开/显示/进入/回到/返回）+ 导航词命中即确定性直通切页，不进 LLM。
// 注意先于 kRules 匹配："打开行车记录"（切页）优先于"行车记录"（状态查询），
// 而"打开录像"不受影响（"录像"不是导航词）。

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::vector<std::pair<std::string, std::string>> parse_nav_words(const std::string& conf) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream iss(conf);
    std::string item;
    while (iss >> item) {
        auto colon = item.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= item.size()) continue;
        out.emplace_back(item.substr(0, colon), item.substr(colon + 1));
    }
    return out;
}

// 返回 target 页面 id；无命中返回 ""
std::string match_nav(const std::string& query,
                      const std::vector<std::pair<std::string, std::string>>& nav_words) {
    static const char* kVerbs[] = {"打开", "显示", "进入", "回到", "返回", "切换到"};
    bool has_verb = false;
    for (const char* v : kVerbs)
        if (query.find(v) != std::string::npos) { has_verb = true; break; }
    if (!has_verb) return "";
    for (const auto& [word, target] : nav_words)
        if (query.find(word) != std::string::npos) return target;
    return "";
}

// ---------- 下游请求构造 ----------

nlohmann::json build_llm_request(const std::string& query, const std::string& rag_context,
                                 const std::string& session_id) {
    nlohmann::json req{{"query", query}, {"session_id", session_id}};
    if (!rag_context.empty()) req["rag_context"] = rag_context;
    return req;
}

nlohmann::json build_llm_agent_request(const std::string& query, const std::string& rag_context,
                                       bool allow_tool, const std::string& tool_result,
                                       const std::string& tool_request,
                                       const std::string& session_id) {
    nlohmann::json req = build_llm_request(query, rag_context, session_id);
    if (allow_tool) req["allow_tool"] = true;
    if (!tool_result.empty()) req["tool_result"] = tool_result;
    if (!tool_request.empty()) req["tool_request"] = tool_request;
    return req;
}

// LLM tool_call 响应 → tool_bus 请求；缺失字段回退规则兜底
std::string build_tool_request_from_llm(const std::string& llm_resp, const std::string& query) {
    auto resp = jparse(llm_resp);
    const std::string tool = jstr(resp, "tool");
    const std::string action = jstr(resp, "tool_action");
    if (tool.empty() || action.empty()) return build_tool_request(query);

    nlohmann::json req{{"tool", tool}, {"action", action}};
    nlohmann::json params = resp.value("params", nlohmann::json::object());
    if (params.is_object()) {
        // LLM 参数名别名归一（prj1 口径保留：target_temp→temp 等）
        const std::pair<const char*, std::vector<const char*>> aliases[] = {
            {"temp", {"temp", "target_temp"}},
            {"mode", {"mode"}},
            {"level", {"level"}},
            {"percent", {"percent", "position"}},
            {"window", {"window", "target_window"}},
        };
        for (const auto& [want, names] : aliases) {
            for (const char* n : names) {
                auto it = params.find(n);
                if (it != params.end()) {
                    req[want] = *it;
                    break;
                }
            }
        }
    }
    return req.dump();
}

const char* type_name(edge_llm_rag::QueryClassification::QueryType t) {
    switch (t) {
        case edge_llm_rag::QueryClassification::FACTUAL_QUERY:    return "FACTUAL";
        case edge_llm_rag::QueryClassification::EMERGENCY_QUERY:  return "EMERGENCY";
        case edge_llm_rag::QueryClassification::EXPLICIT_COMMAND: return "EXPLICIT_CMD";
        case edge_llm_rag::QueryClassification::COMPLEX_QUERY:    return "COMPLEX";
        case edge_llm_rag::QueryClassification::CREATIVE_QUERY:   return "CREATIVE";
        default:                                                   return "UNKNOWN";
    }
}

}  // namespace

int main() {
    if (!vox::config::load()) VOX_WARN("未找到 voxdrive.conf，端口使用内置默认值");

    zmq_component::ZmqServer server(vox::config::bind_endpoint("port.intent_router", "6666"));
    zmq_component::ZmqPub status_pub(vox::config::bind_endpoint("port.intent_router_pub", "6671"));
    edge_llm_rag::QueryClassifier classifier;

    zmq_component::ZmqClient rag_client(vox::config::connect_endpoint("port.rag", "6667"));
    zmq_component::ZmqClient llm_client(vox::config::connect_endpoint("port.llm", "6668"));
    zmq_component::ZmqClient tool_client(vox::config::connect_endpoint("port.tool_bus", "6669"));

    // ── 语义通路接线（E1 双路路由）：意图中心 + 查询编码 ──
    // 编码走 RAG 服务 embed 端点；每次请求新建 ZmqClient：REQ 状态机被超时破坏后
    // 下次调用可自愈（复用被毒化的 socket 会 EFSM），且 2s 短超时保证编码故障
    // 只拖慢单条查询、不阻塞主循环（失败=语义通路返回 UNKNOWN，规则单路兜底）。
    edge_llm_rag::SemanticRouter semantic_router;
    const std::string centers_path = vox::config::get("router.intent_centers", "");
    bool semantic_ready = false;
    if (centers_path.empty() || !semantic_router.load_from_file(centers_path)) {
        VOX_WARN("意图中心未加载（%s）——语义通路禁用，仅规则单路",
                 centers_path.empty() ? "conf 未配置 router.intent_centers"
                                      : centers_path.c_str());
    } else if (vox::config::get("router.semantic_enabled", "1") == "0") {
        VOX_WARN("router.semantic_enabled=0 ——语义通路被 conf 关闭");
    } else {
        semantic_ready = true;
    }
    if (semantic_ready) {
        std::string names;
        for (const auto& i : semantic_router.intents_view()) names += i.name + " ";
        VOX_INFO("[semantic] %zu intents (dim=%d): %s", semantic_router.intent_count(),
                 semantic_router.dim(), names.c_str());
    }
    auto encode_via_rag = [ep = vox::config::connect_endpoint("port.rag", "6667")](
                              const std::string& q) -> std::vector<float> {
        zmq_component::ZmqClient c(ep);
        c.setTimeout(2000);
        std::string resp = c.request(
            nlohmann::json{{"op", "embed"}, {"text", q}}.dump());
        auto j = jparse(resp);
        if (!j.is_object()) return {};
        auto it = j.find("vector");
        if (it == j.end() || !it->is_array()) return {};
        return it->get<std::vector<float>>();
    };
    if (semantic_ready) classifier.attach_semantic(&semantic_router, encode_via_rag);

    VOX_INFO("listening %s, pub %s",
             vox::config::bind_endpoint("port.intent_router", "").c_str(),
             vox::config::bind_endpoint("port.intent_router_pub", "").c_str());

    // TTS 异步播报：独立线程发送，失败不阻塞主流程（TTS 未启动时仅记日志）
    const std::string tts_ep = vox::config::connect_endpoint("port.tts_text", "7777");
    auto send_tts_async = [&](const std::string& text) {
        if (text.empty()) return;
        // UI v2.0 语音上屏：播报文本经 6671 PUB（dashboard 气泡，设计规范 7.2）
        status_pub.publish(nlohmann::json{{"service", "router"}, {"status", "tts_say"},
                                          {"tts_text", text}, {"ts", now_ms()}}
                               .dump());
        std::thread([text, tts_ep]() {
            try {
                zmq_component::ZmqClient tts_client(tts_ep);
                tts_client.request(json_escape(text) + " END");
            } catch (...) {
                VOX_WARN("TTS 异步请求失败（服务未启动？）");
            }
        }).detach();
    };

    // LLM(agent) 循环：allow_tool 时若 LLM 选择 tool_call，则执行工具并把结果回灌 LLM 生成最终答复
    auto run_llm_tool_loop = [&](const std::string& query, const std::string& rag_text,
                                 const std::string& status_prefix, const std::string& session_id,
                                 std::string& tts_text_out) {
        status_pub.publish(
            nlohmann::json{{"service", "router"}, {"status", status_prefix + " -> LLM(agent)"}}
                .dump());
        std::string llm_resp = llm_client.request(
            build_llm_agent_request(query, rag_text, true, "", "", session_id).dump());
        VOX_INFO("[llm(agent)] %.160s", llm_resp.c_str());

        if (jstr(jparse(llm_resp), "mode") == "tool_call") {
            status_pub.publish(nlohmann::json{{"service", "router"},
                                              {"status", status_prefix + " -> LLM -> Tool -> LLM -> TTS"}}
                                   .dump());
            std::string tool_req = build_tool_request_from_llm(llm_resp, query);
            VOX_INFO("[-> tool] %s", tool_req.c_str());
            std::string tool_resp = tool_client.request(tool_req);
            VOX_INFO("[tool ->] %.160s", tool_resp.c_str());

            std::string tool_result = jstr(jparse(tool_resp), "result");
            if (tool_result.empty()) tool_result = tool_resp;

            llm_resp = llm_client.request(
                build_llm_agent_request(query, rag_text, false, tool_result, tool_req, session_id)
                    .dump());
            VOX_INFO("[llm(final)] %.160s", llm_resp.c_str());
        }

        tts_text_out = jstr(jparse(llm_resp), "text");
        return llm_resp;
    };

    // ── 语音导航词表（conf 静态表；插件化 dashboard 的 nav_words 汇总，P2 简化口径）──
    const auto nav_words = parse_nav_words(
        vox::config::get("dashboard.nav_words",
                         "行车记录:dashcam 记录仪:dashcam 车控:vehicle 车辆控制:vehicle "
                         "状态:status 设置:settings 主页:home 首页:home 桌面:home"));

    while (true) {
        const std::string text = server.receive();
        VOX_INFO("[asr ->] %s", text.c_str());
        const std::string session_id = "default";

        // ── UI v2.0 语音上屏：用户原文经 6671 PUB（dashboard 字幕，设计规范 7.2）──
        status_pub.publish(nlohmann::json{{"service", "router"}, {"status", "asr_final"},
                                          {"asr_text", text}, {"ts", now_ms()}}
                               .dump());

        // ── 语音导航确定性直通（切页不进 LLM，设计规范 7.3）──
        if (std::string nav_target = match_nav(text, nav_words); !nav_target.empty()) {
            VOX_INFO("[nav] %s -> %s", text.c_str(), nav_target.c_str());
            status_pub.publish(nlohmann::json{{"service", "router"}, {"status", "nav"},
                                              {"target", nav_target}, {"ts", now_ms()}}
                                   .dump());
            const std::string back =
                (nav_target == "home") ? "好的，已回到主页" : "好的，已打开页面";
            nlohmann::json reply{{"found", true}, {"mode", "answer"}, {"text", back},
                                 {"nav", nav_target}};
            server.send(reply.dump());
            send_tts_async(back);
            continue;
        }

        // ── dashcam 域确定性直通（D2/R7）：命中即执行工具，LLM 只组织播报 ──
        // 真设备控制不交给 LLM 猜：Qwen2.5-1.5B 在 7 个工具间选择不可靠（实测
        // "关闭预览"被选成 window_control.close_all、"还剩多少存储"被选成
        // sensor_read 拿 mock 数据编数）。直通后单次 LLM 调用（allow_tool=false）
        // 基于真实工具结果生成答复，延迟也省一轮 LLM。
        if (const Rule* r = match_first_rule(text);
            r != nullptr && std::strcmp(r->tool, "dashcam") == 0) {
            status_pub.publish(nlohmann::json{{"service", "router"},
                                              {"status", "DASHCAM -> Tool -> LLM -> TTS"}}
                                   .dump());
            const std::string tool_req = build_req_from_rule(*r, text);
            VOX_INFO("[dashcam-direct] kw=%s %s", r->kw, tool_req.c_str());
            std::string tool_resp = tool_client.request(tool_req);
            VOX_INFO("[tool ->] %.160s", tool_resp.c_str());
            std::string tool_result = jstr(jparse(tool_resp), "result");
            if (tool_result.empty()) tool_result = tool_resp;

            std::string llm_resp = llm_client.request(
                build_llm_agent_request(text, "", false, tool_result, tool_req, session_id)
                    .dump());
            VOX_INFO("[llm(final)] %.160s", llm_resp.c_str());
            std::string tts_text = jstr(jparse(llm_resp), "text");
            server.send(llm_resp);
            send_tts_async(tts_text);
            if (!tts_text.empty()) VOX_INFO("[tts] text sent (%zu chars)", tts_text.size());
            continue;
        }

        auto cls = classifier.classify_query(text);
        auto cfg = classifier.build_route_config(cls);
        const char* type_str = type_name(cls.query_type);
        VOX_INFO("type=%s rag=%d llm=%d tool=%d | %s", type_str, cfg.use_rag, cfg.use_llm,
                 cfg.use_tool_direct, cls.reasoning.c_str());

        std::string reply;

        // ── 快路径: RAG → TTS ──
        if (cfg.use_rag && !cfg.use_llm) {
            status_pub.publish(
                nlohmann::json{{"service", "router"},
                               {"status", std::string(type_str) + " -> RAG -> TTS"}}.dump());
            std::string rag_resp = rag_client.request(text);
            VOX_INFO("[rag ->] %.120s", rag_resp.c_str());

            std::string tts_text = jstr(jparse(rag_resp), "text");
            if (!tts_text.empty()) {
                send_tts_async(tts_text);
                VOX_INFO("[tts] text sent (%zu chars)", tts_text.size());
            }
            server.send(rag_resp);
            continue;
        }
        // ── 显式指令: LLM(agent) -> [Tool] -> LLM -> TTS ──
        if (cfg.use_tool_direct) {
            std::string tts_text;
            reply = run_llm_tool_loop(text, "", type_str, session_id, tts_text);
            server.send(reply);
            if (!tts_text.empty()) {
                send_tts_async(tts_text);
                VOX_INFO("[tts] text sent (%zu chars)", tts_text.size());
            }
            continue;
        }
        // ── 慢路径: RAG + LLM(agent) → TTS ──
        if (cfg.use_rag && cfg.use_llm) {
            std::string rag_resp = rag_client.request(text);
            std::string rag_text = jstr(jparse(rag_resp), "text");
            VOX_INFO("[rag ->] context: %.80s", rag_text.c_str());
            std::string status_prefix = (cls.query_type == edge_llm_rag::QueryClassification::UNKNOWN_QUERY ||
                                         cls.query_type == edge_llm_rag::QueryClassification::COMPLEX_QUERY)
                                            ? std::string("RAG -> ") + type_str
                                            : std::string(type_str);
            std::string tts_text;
            reply = run_llm_tool_loop(text, rag_text, status_prefix, session_id, tts_text);
            server.send(reply);
            if (!tts_text.empty()) {
                send_tts_async(tts_text);
                VOX_INFO("[tts] text sent (%zu chars)", tts_text.size());
            }
            continue;
        }
        // ── 慢路径: LLM → TTS ──
        if (cfg.use_llm) {
            status_pub.publish(
                nlohmann::json{{"service", "router"},
                               {"status", std::string(type_str) + " -> LLM -> TTS"}}.dump());
            std::string llm_resp =
                llm_client.request(build_llm_request(text, "", session_id).dump());
            VOX_INFO("[llm ->] %.120s", llm_resp.c_str());

            std::string tts_text = jstr(jparse(llm_resp), "text");
            if (!tts_text.empty()) {
                send_tts_async(tts_text);
                VOX_INFO("[tts] text sent (%zu chars)", tts_text.size());
            }
            server.send(llm_resp);
            continue;
        }
        // ── 兜底 ──
        server.send("{\"found\":false,\"text\":\"无法处理该请求\"}");
    }
}
