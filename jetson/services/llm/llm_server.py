#!/usr/bin/env python3
"""LLM 服务 — ZMQ REP :port.llm，代理 llama-server HTTP API。

请求字段：query / rag_context / allow_tool / tool_result / tool_request / session_id
应答协议（与 prj1 兼容）：
  {"found":true,"mode":"answer","text":"...","tokens":N}
  {"found":true,"mode":"tool_call","tool":"...","tool_action":"...","params":{},"reason":"..."}
约束生成 respond/tool_call 两类 JSON，解析失败自动重试一次，再失败走关键词兜底。
"""

import json
import os
import pathlib
import sys
import urllib.error
import urllib.request
from collections import defaultdict, deque

JETSON_ROOT = pathlib.Path(__file__).resolve().parents[2]  # services/llm/ → jetson/
sys.path.insert(0, str(JETSON_ROOT))

import zmq  # noqa: E402

from common import vox_config, vox_log  # noqa: E402

vox_config.load()

# 环境变量优先于 conf（测试/临时覆盖用）
LLAMA_SERVER_URL = os.environ.get("LLAMA_SERVER_URL") or \
    vox_config.get("llm.server_url", "http://127.0.0.1:8080")
MODEL_NAME = os.environ.get("LLM_MODEL") or vox_config.get("llm.model_name", "qwen2.5-1.5b")
HTTP_TIMEOUT_S = vox_config.get_int("timeout.llm_http_s", 120)
MAX_HISTORY_TURNS = vox_config.get_int("llm.max_history_turns", 6)
MAX_SESSIONS = vox_config.get_int("llm.max_sessions", 8)

# 会话历史：单会话按轮数封顶（deque），会话总数封顶（防长稳内存增长）
SESSION_HISTORY = defaultdict(lambda: deque(maxlen=MAX_HISTORY_TURNS * 2))

TOOL_SPEC = """
可用工具如下：
1. climate_control: turn_on, turn_off, set_temp, set_mode, set_fan
2. window_control: open_all, close_all, fl, fr, rl, rr, set
3. sunroof_control: open, close, tilt
4. seat_heater: driver_on, driver_off, passenger_on, passenger_off, on, off
5. camera_capture: front, rear, left, right, record_on, record_off
6. sensor_read: read
7. dashcam: 行车记录仪（跨板真实设备）。动作：status=查询录像/存储/预览状态，
   record_on/record_off=开始/停止录像, preview_on/preview_off=打开/关闭预览推流,
   snapshot=抓拍一张照片并保存。
   用户问"还剩多少存储/存储空间/现在录着吗/录像状态/行车记录仪怎么样"用 status；
   说"拍张照/拍照/抓拍"用 snapshot；要求开始或停止录像/预览用对应开关动作。
"""

# R6 TODO：本表与 intent_router 规则表/TOOL_SPEC 三处重复，届时合一为单一工具注册表
FALLBACK_TOOL_RULES = [
    ("dashcam", "status", ["行车记录", "存储", "录着", "在录像"]),
    ("dashcam", "snapshot", ["拍张照", "拍照", "抓拍"]),
    ("climate_control", "set_temp", ["空调", "温度", "几度"]),
    ("window_control", "open_all", ["车窗"]),
    ("sunroof_control", "open", ["天窗"]),
    ("seat_heater", "on", ["座椅加热"]),
    ("camera_capture", "rear", ["倒车影像", "后视"]),
    ("sensor_read", "read", ["车速", "油量", "胎压", "里程", "发动机温度"]),
]


def chat_completion(messages, max_tokens=256, temperature=0.3):
    payload = json.dumps(
        {
            "model": MODEL_NAME,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature,
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        f"{LLAMA_SERVER_URL}/v1/chat/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.URLError as exc:
        return {"error": str(exc)}


def try_parse_json(text):
    text = (text or "").strip()
    if not text:
        return None
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        pass

    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1 and end > start:
        try:
            return json.loads(text[start : end + 1])
        except json.JSONDecodeError:
            return None
    return None


def infer_sensor_name(query):
    if "车速" in query:
        return "speed"
    if "油量" in query:
        return "fuel_percent"
    if "胎压" in query:
        return "tire_pressure"
    if "里程" in query:
        return "mileage"
    if "发动机温度" in query:
        return "engine_temp"
    return ""


def infer_window_target(query):
    if "左前" in query:
        return "fl"
    if "右前" in query:
        return "fr"
    if "左后" in query:
        return "rl"
    if "右后" in query:
        return "rr"
    return ""


def normalize_tool_params(tool, action, params):
    params = dict(params or {})

    if tool == "climate_control":
        if "temperature" in params and "temp" not in params:
            params["temp"] = params["temperature"]
        if "target_temp" in params and "temp" not in params:
            params["temp"] = params["target_temp"]
        if action == "turn_on" and "temp" in params:
            action = "set_temp"
        if action == "set_mode" and params.get("mode") in {"cooling", "cold"}:
            params["mode"] = "cool"
        if action == "set_mode" and params.get("mode") in {"heating", "warm"}:
            params["mode"] = "heat"

    elif tool == "window_control":
        if "position" in params and "percent" not in params:
            params["percent"] = params["position"]
        if "target_window" in params and "window" not in params:
            params["window"] = params["target_window"]

    elif tool == "seat_heater":
        if action == "on" and params.get("seat") == "driver":
            action = "driver_on"
        elif action == "on" and params.get("seat") == "passenger":
            action = "passenger_on"
        elif action == "off" and params.get("seat") == "driver":
            action = "driver_off"
        elif action == "off" and params.get("seat") == "passenger":
            action = "passenger_off"

    return action, params


def normalize_tool_call(payload):
    if not isinstance(payload, dict):
        return None
    action = payload.get("action")
    if action != "tool_call":
        return None

    tool = payload.get("tool", "")
    tool_action = payload.get("tool_action", "")
    params = payload.get("params", {})
    if not isinstance(params, dict):
        params = {}

    if not tool or not tool_action:
        return None

    tool_action, params = normalize_tool_params(tool, tool_action, params)

    return {
        "found": True,
        "mode": "tool_call",
        "tool": tool,
        "tool_action": tool_action,
        "params": params,
        "reason": payload.get("reason", ""),
    }


def extract_number_before_unit(text, unit):
    idx = text.find(unit)
    if idx <= 0:
        return ""
    start = idx
    while start > 0 and text[start - 1].isdigit():
        start -= 1
    return text[start:idx]


def infer_tool_call(query):
    params = {}
    for tool, action, keywords in FALLBACK_TOOL_RULES:
        if any(keyword in query for keyword in keywords):
            if tool == "climate_control" and action == "set_temp":
                temp = extract_number_before_unit(query, "度")
                if temp:
                    params["temp"] = temp
            if tool == "sensor_read":
                sensor = infer_sensor_name(query)
                if sensor:
                    params["sensor"] = sensor
            if tool == "window_control":
                target_window = infer_window_target(query)
                percent = extract_number_before_unit(query, "%")
                if target_window and percent:
                    action = "set"
                    params["window"] = target_window
                    params["percent"] = percent

            action, params = normalize_tool_params(tool, action, params)
            return {
                "found": True,
                "mode": "tool_call",
                "tool": tool,
                "tool_action": action,
                "params": params,
                "reason": "heuristic_fallback",
            }
    return None


def retry_tool_json(messages, raw_content):
    retry_messages = list(messages)
    retry_messages.append({"role": "assistant", "content": raw_content})
    retry_messages.append(
        {
            "role": "user",
            "content": (
                "你上一条回复不符合要求。"
                "请严格只输出一个合法 JSON，不要解释，不要 Markdown，不要额外文本。"
                "格式只能是 {\"action\":\"respond\",\"text\":\"...\"} 或 "
                "{\"action\":\"tool_call\",\"tool\":\"...\",\"tool_action\":\"...\",\"params\":{},\"reason\":\"...\"}。"
            ),
        }
    )
    return chat_completion(retry_messages, max_tokens=192, temperature=0.1)


def build_messages(
    query,
    rag_context="",
    allow_tool=False,
    tool_result="",
    tool_request="",
    history=None,
):
    history = list(history or [])

    if tool_result:
        system_prompt = (
            "你是一个车载语音助手。你已经拿到了工具执行结果。"
            "请基于用户问题、车辆手册参考知识和工具结果，生成最终给用户播报的中文回复。"
            "回复必须口语化、准确、控制在100字以内。"
            "只输出一个 JSON：{\"action\":\"respond\",\"text\":\"...\"}。"
        )
        messages = [{"role": "system", "content": system_prompt}]
        messages.extend(history)
        user_prompt = (
            f"当前用户问题：{query}\n"
            f"车辆手册参考知识：{rag_context or '无'}\n"
            f"工具请求：{tool_request or '无'}\n"
            f"工具结果：{tool_result}"
        )
        messages.append({"role": "user", "content": user_prompt})
        return messages

    if allow_tool:
        system_prompt = (
            "你是一个车载语音助手。"
            "如果问题需要读取车辆状态或执行车控命令，就选择 tool_call；"
            "如果不需要工具，直接回答。"
            "只能输出 JSON，不能输出解释文本。"
            "可选格式二选一："
            "{\"action\":\"respond\",\"text\":\"...\"}"
            "或"
            "{\"action\":\"tool_call\",\"tool\":\"...\",\"tool_action\":\"...\",\"params\":{},\"reason\":\"...\"}。"
            + TOOL_SPEC
        )
        messages = [{"role": "system", "content": system_prompt}]
        messages.extend(history)
        user_prompt = f"当前用户问题：{query}\n车辆手册参考知识：{rag_context or '无'}"
        messages.append({"role": "user", "content": user_prompt})
        return messages

    if rag_context:
        system_prompt = (
            "你是一个车载语音助手。请根据以下车辆手册知识，用简洁的中文回答。"
            "回答要口语化，适合语音播报，控制在100字以内。\n\n"
            f"车辆手册参考知识：\n{rag_context}"
        )
    else:
        system_prompt = (
            "你是一个车载语音助手。用简洁的中文回答用户问题。"
            "回答要口语化，适合语音播报，控制在100字以内。"
        )
    messages = [{"role": "system", "content": system_prompt}]
    messages.extend(history)
    messages.append({"role": "user", "content": query})
    return messages


def append_history(session_id, role, content):
    if not content:
        return
    SESSION_HISTORY[session_id].append({"role": role, "content": content})
    # 会话总数超限时淘汰最旧会话（dict 按插入序；单会话 "default" 场景不触发）
    while len(SESSION_HISTORY) > MAX_SESSIONS:
        SESSION_HISTORY.pop(next(iter(SESSION_HISTORY)))


def reset_history(session_id):
    SESSION_HISTORY.pop(session_id, None)


def parse_llm_reply(content, usage, query="", allow_tool=False, tool_result=""):
    parsed = try_parse_json(content)

    if allow_tool and not tool_result:
        tool_call = normalize_tool_call(parsed)
        if tool_call:
            tool_call["tokens"] = usage.get("total_tokens", 0)
            tool_call["decision_source"] = "llm_direct_tool_call"
            return tool_call
        inferred = infer_tool_call(query)
        if inferred:
            inferred["tokens"] = usage.get("total_tokens", 0)
            inferred["reason"] = "minimal_fallback"
            inferred["decision_source"] = "minimal_fallback"
            return inferred

    if isinstance(parsed, dict) and parsed.get("action") == "respond":
        content = parsed.get("text", "")

    return {
        "found": True,
        "mode": "answer",
        "text": content.strip(),
        "tokens": usage.get("total_tokens", 0),
    }


def main():
    log = vox_log.setup("llm")

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REP)
    sock.bind(vox_config.bind_endpoint("port.llm", "6668"))

    status_ctx = zmq.Context()
    status_sock = status_ctx.socket(zmq.PUB)
    status_sock.connect(vox_config.connect_endpoint("port.intent_router_pub", "6671"))

    log.info("listening %s, backend %s (model %s)",
             vox_config.bind_endpoint("port.llm", ""), LLAMA_SERVER_URL, MODEL_NAME)

    while True:
        try:
            request = sock.recv_string()
        except KeyboardInterrupt:
            break

        log.info("request: %.160s", request)

        try:
            req_json = json.loads(request)
            query = req_json.get("query", request)
            rag_context = req_json.get("rag_context", "")
            allow_tool = bool(req_json.get("allow_tool", False))
            tool_result = req_json.get("tool_result", "")
            tool_request = req_json.get("tool_request", "")
            session_id = req_json.get("session_id", "default")
        except (json.JSONDecodeError, ValueError):
            query = request
            rag_context = ""
            allow_tool = False
            tool_result = ""
            tool_request = ""
            session_id = "default"

        if query.strip() in {"清空对话", "重置对话", "reset conversation"}:
            reset_history(session_id)
            sock.send_string(json.dumps(
                {"found": True, "mode": "answer", "text": "好的，已清空当前对话上下文。", "tokens": 0},
                ensure_ascii=False))
            continue

        history = SESSION_HISTORY[session_id]

        messages = build_messages(
            query,
            rag_context=rag_context,
            allow_tool=allow_tool,
            tool_result=tool_result,
            tool_request=tool_request,
            history=history,
        )
        result = chat_completion(messages)

        if "error" in result:
            reply = json.dumps(
                {"found": False, "mode": "error", "text": f"[LLM Error: {result['error']}]"},
                ensure_ascii=False,
            )
            log.error("backend error: %s", result["error"])
        else:
            content = result.get("choices", [{}])[0].get("message", {}).get("content", "")
            usage = result.get("usage", {})

            if allow_tool and not tool_result:
                first_pass = parse_llm_reply(
                    content, usage, query=query, allow_tool=allow_tool, tool_result=tool_result
                )
                if first_pass.get("mode") == "answer" and try_parse_json(content) is None:
                    retry_result = retry_tool_json(messages, content)
                    if "error" not in retry_result:
                        log.info("工具 JSON 不合规，重试一次")
                        content = retry_result.get("choices", [{}])[0].get("message", {}).get("content", "")
                        retry_usage = retry_result.get("usage", {})
                        usage = {
                            "total_tokens": usage.get("total_tokens", 0) + retry_usage.get("total_tokens", 0)
                        }

            parsed_reply = parse_llm_reply(
                content, usage, query=query, allow_tool=allow_tool, tool_result=tool_result
            )
            if allow_tool and not tool_result and "decision_source" not in parsed_reply and parsed_reply.get("mode") == "tool_call":
                parsed_reply["decision_source"] = "llm_json_retry"
            reply = json.dumps(parsed_reply, ensure_ascii=False)
            log.info("response mode=%s, %d tokens: %.80s",
                     parsed_reply.get("mode", "answer"), usage.get("total_tokens", 0),
                     parsed_reply.get("text", ""))

            if tool_result:
                append_history(session_id, "user", query)
                append_history(session_id, "assistant", parsed_reply.get("text", ""))
            elif not allow_tool:
                append_history(session_id, "user", query)
                append_history(session_id, "assistant", parsed_reply.get("text", ""))

            try:
                status = parsed_reply.get("mode", "answer")
                status_sock.send_string(
                    json.dumps(
                        {
                            "service": "llm",
                            "status": f"{status} {usage.get('total_tokens', 0)} tokens",
                        },
                        ensure_ascii=False,
                    ),
                    zmq.NOBLOCK,
                )
            except zmq.ZMQError:
                pass  # PUB 非阻塞发送，队列满丢弃状态消息可接受

        sock.send_string(reply)

    sock.close()
    ctx.term()


if __name__ == "__main__":
    main()
