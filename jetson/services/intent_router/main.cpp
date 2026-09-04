// intent_router — 意图路由服务
//
// REP : port.intent_router（ASR 文本/查询进入）  PUB : port.intent_router_pub（全局状态总线）
// 下游：RAG :port.rag · LLM :port.llm · ToolBus :port.tool_bus · TTS :port.tts_text（异步线程）
//
// 协议（与 prj1 兼容）：
//   入：纯文本 query；出：命中路径下游服务的应答 JSON 原样透传
//   TTS 异步推送："<JSON转义文本> END"（TTS 以 " END" 为单条播报结束标记）
//
// 语义路由钩子（E1 待接）：classify_query 目前仅规则通路；语义通路（意图中心向量
// 余弦兜底）接口预留于 semantic_router.h + build_intent_centers.py，接入时不动本文件主流程。
#define VOX_LOG_TAG "intent_router"
#include "../../common/vox_log.h"
#include "../../common/vox_config.h"
#include "../../common/json.hpp"

#include "query_classifier.h"
#include "ZmqClient.h"
#include "ZmqPub.h"
#include "ZmqServer.h"

#include <cstring>
#include <string>
#include <thread>

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

// 规则兜底：LLM 未给出 tool_call 细节时按关键词直接构造工具请求
std::string build_tool_request(const std::string& query) {
    for (const Rule& r : kRules) {
        if (query.find(r.kw) == std::string::npos) continue;
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
    return "{\"tool\":\"sensor_read\",\"action\":\"read\"}";  // 兜底读传感器（保持 prj1 行为）
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
    VOX_INFO("listening %s, pub %s",
             vox::config::bind_endpoint("port.intent_router", "").c_str(),
             vox::config::bind_endpoint("port.intent_router_pub", "").c_str());

    // TTS 异步播报：独立线程发送，失败不阻塞主流程（TTS 未启动时仅记日志）
    const std::string tts_ep = vox::config::connect_endpoint("port.tts_text", "7777");
    auto send_tts_async = [&](const std::string& text) {
        if (text.empty()) return;
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

    while (true) {
        const std::string text = server.receive();
        VOX_INFO("[asr ->] %s", text.c_str());
        const std::string session_id = "default";

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
