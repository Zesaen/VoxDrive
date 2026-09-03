#!/usr/bin/env bash
# intent_router 板上自测：cmake 编译 → mock LLM/RAG + 真 tool_bus + router 拉起 →
# 断言 respond 透传 / tool_call 两段循环 / 状态总线广播 → 清理
# 前置：sync_to_board.sh 已同步（tool_bus 已编译过）
# 环境变量: VOX_BOARD(默认 nvidia)、VOX_REMOTE_DIR(默认 ~/Desktop/VoxDrive/jetson)
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC_DIR="$REMOTE_DIR/services/intent_router"

echo "[selftest:intent_router] 编译"
ssh "$BOARD" "set -e
  mkdir -p '$SVC_DIR/build'
  cd '$SVC_DIR/build'
  cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build . -j\$(nproc) >/dev/null"

echo "[selftest:intent_router] 拉起 mock LLM/RAG + 真 tool_bus + router 并断言"
ssh "$BOARD" "pkill -x intent_router 2>/dev/null || true; pkill -x tool_bus 2>/dev/null || true; sleep 0.3"
ssh "$BOARD" "/usr/bin/python3 -u - \
  '$SVC_DIR/build/intent_router' \
  '$REMOTE_DIR/services/tool_bus/build/tool_bus'" <<'PYEOF'
import json
import signal
import subprocess
import sys
import time

import zmq

router_bin, toolbus_bin = sys.argv[1], sys.argv[2]

# ---- mock 下游：LLM 按 query 内容切换 tool_call/respond；RAG 固定答复 ----
llm_calls = []   # 记录 LLM 收到的请求（断言 tool_result 回灌）

def mock_llm():
    import threading
    ctx = zmq.Context()
    rep = ctx.socket(zmq.REP)
    rep.bind("tcp://*:6668")
    rag = ctx.socket(zmq.REP)
    rag.bind("tcp://*:6667")
    poller = zmq.Poller()
    poller.register(rep, zmq.POLLIN)
    poller.register(rag, zmq.POLLIN)
    state = {"tool_call_sent": False}

    def handle_llm(req_text):
        llm_calls.append(json.loads(req_text))
        if not state["tool_call_sent"]:
            state["tool_call_sent"] = True
            return json.dumps({"mode": "tool_call", "tool": "climate_control",
                               "tool_action": "set_temp",
                               "params": {"target_temp": 25}})   # 故意用别名 target_temp
        return json.dumps({"mode": "respond", "text": "好的，空调已调整"})

    while True:
        socks = dict(poller.poll(50))
        if rep in socks:
            rep.send_string(handle_llm(rep.recv_string()))
        if rag in socks:
            rag.recv_string()
            rep_text = json.dumps({"found": True, "text": "手册说：每5000公里保养"})
            rag.send_string(rep_text)

    ctx.term()

import threading
t = threading.Thread(target=mock_llm, daemon=True)
t.start()
time.sleep(0.5)

toolbus_log = open("/tmp/tool_bus.log", "w")
toolbus = subprocess.Popen([toolbus_bin], stdout=toolbus_log, stderr=toolbus_log)
router_log = open("/tmp/intent_router.log", "w")
router = subprocess.Popen([router_bin], stdout=router_log, stderr=router_log)
ctx = zmq.Context()
try:
    time.sleep(1.0)
    if router.poll() is not None:
        sys.exit("intent_router 启动即退出，查看 /tmp/intent_router.log")

    sub = ctx.socket(zmq.SUB)   # 状态总线 6671
    sub.setsockopt(zmq.SUBSCRIBE, b"")
    sub.setsockopt(zmq.RCVTIMEO, 3000)
    sub.setsockopt(zmq.LINGER, 0)
    sub.connect("tcp://localhost:6671")

    def ask(query):
        s = ctx.socket(zmq.REQ)
        s.setsockopt(zmq.RCVTIMEO, 10000)
        s.setsockopt(zmq.LINGER, 0)
        s.connect("tcp://localhost:6666")
        try:
            s.send_string(query)
            return json.loads(s.recv_string())
        finally:
            s.close()

    time.sleep(0.5)  # 等 SUB 订阅传播

    # 1) 显式指令 → LLM tool_call（别名 target_temp 归一为 temp）→ tool_bus → LLM 最终答复
    r = ask("把空调温度调到25度")
    assert r["mode"] == "respond" and r["text"] == "好的，空调已调整", r
    assert len(llm_calls) >= 2, "应有两次 LLM 调用（agent + 最终）"
    second = llm_calls[1]
    assert second.get("tool_result"), "第二次 LLM 请求应携带 tool_result"
    toolbus_state = None  # 工具确实执行过：tool_result 非空且含"度"
    assert "25" in second["tool_result"], second["tool_result"]

    # 2) 再次指令（mock LLM 直接 respond）：透传
    r = ask("打开车窗")
    assert r["mode"] == "respond", r

    # 3) 状态总线有 router 状态广播
    seen = False
    for _ in range(3):
        try:
            m = json.loads(sub.recv_string())
            if m.get("service") == "router" and "status" in m:
                seen = True
                break
        except zmq.Again:
            break
    assert seen, "未收到 router 状态广播"

    print("SELFTEST_INTENT_ROUTER PASS")
finally:
    for p in (router, toolbus):
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    router_log.close()
    toolbus_log.close()
    ctx.destroy()
PYEOF
echo "[selftest:intent_router] 全部 PASS"
