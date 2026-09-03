#!/usr/bin/env bash
# run_regression.sh — 全栈回归：4 条 canned 查询打 Intent Router(6666)，
# 断言每条应答为合法 JSON 且 mode ∈ {answer, tool_call}，输出逐条延迟。
# 前置：start_core.sh 已启动全栈（在板上运行本脚本）。
set -u

JETSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONF="$JETSON_DIR/config/voxdrive.conf"
P_ROUTER=$(awk -F= '$1 ~ /^port.intent_router[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
P_TTS_BLOCK=$(awk -F= '$1 ~ /^port.tts_block[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")

/usr/bin/python3 -u - "$P_ROUTER" "$P_TTS_BLOCK" <<'PY'
import json
import sys
import time

import zmq

router_port, block_port = sys.argv[1], sys.argv[2]
QUERIES = [
    "打开空调制冷模式",   # climate → tool_call
    "关闭所有车窗",       # window  → tool_call
    "打开天窗",           # sunroof → tool_call
    "查看胎压和车速",     # sensor  → tool_call/respond
]

ctx = zmq.Context()

passed = 0
for i, q in enumerate(QUERIES, 1):
    t0 = time.time()
    try:
        # 每条查询新建 REQ：避免超时后 REQ 状态机卡死影响后续查询
        router = ctx.socket(zmq.REQ)
        router.setsockopt(zmq.RCVTIMEO, 30000)
        router.setsockopt(zmq.LINGER, 0)
        router.connect(f"tcp://127.0.0.1:{router_port}")
        router.send_string(q)
        reply = json.loads(router.recv_string())
        router.close()
        ms = (time.time() - t0) * 1000
        mode = reply.get("mode", "?")
        ok = reply.get("found") is True and mode in ("answer", "tool_call")
        tag = "PASS" if ok else "FAIL"
        brief = reply.get("text") or f"{reply.get('tool')}.{reply.get('tool_action')}"
        print(f"[{tag}] {i}/4 {q!r} -> mode={mode}, {ms:.0f} ms, {str(brief)[:50]}")
        passed += ok
        # 播报期阻塞门同步（TTS 异步播报；regression 不等 play_end）
        try:
            block = ctx.socket(zmq.REQ)
            block.setsockopt(zmq.RCVTIMEO, 2000)
            block.setsockopt(zmq.LINGER, 0)
            block.connect(f"tcp://127.0.0.1:{block_port}")
            block.send_string("block")
            block.recv_string()
            block.close()
        except zmq.Again:
            pass
    except zmq.Again:
        print(f"[FAIL] {i}/4 {q!r} -> 超时(30s)")
    except (json.JSONDecodeError, ValueError) as e:
        print(f"[FAIL] {i}/4 {q!r} -> 非法应答: {e}")

ctx.destroy()
print(f"\n回归结果: {passed}/{len(QUERIES)} PASS")
sys.exit(0 if passed == len(QUERIES) else 1)
PY
