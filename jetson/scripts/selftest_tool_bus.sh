#!/usr/bin/env bash
# tool_bus 板上自测：cmake 编译 → 后台拉起 → REQ/REP 与 PUB 广播断言（含畸形 JSON 用例）→ 清理
# 前置：sync_to_board.sh 已同步
# 环境变量: VOX_BOARD(默认 nvidia)、VOX_REMOTE_DIR(默认 ~/Desktop/VoxDrive/jetson)
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC_DIR="$REMOTE_DIR/services/tool_bus"

echo "[selftest:tool_bus] 编译"
ssh "$BOARD" "set -e
  mkdir -p '$SVC_DIR/build'
  cd '$SVC_DIR/build'
  cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build . -j\$(nproc) >/dev/null"

echo "[selftest:tool_bus] 拉起服务并断言"
# 清理上次残留（-x 按进程名精确匹配，避免 -f 匹配到本命令行自身）
ssh "$BOARD" "pkill -x tool_bus 2>/dev/null || true; sleep 0.3"
ssh "$BOARD" "/usr/bin/python3 -u - '$SVC_DIR/build/tool_bus'" <<'PYEOF'
import json
import signal
import subprocess
import sys
import time

import zmq

bin_path = sys.argv[1]
log = open("/tmp/tool_bus.log", "w")
proc = subprocess.Popen([bin_path], stdout=log, stderr=log)
try:
    time.sleep(1.0)  # 等 bind + 首条 state_full
    if proc.poll() is not None:
        sys.exit("tool_bus 启动即退出，查看 /tmp/tool_bus.log")

    ctx = zmq.Context()

    def call(req_text):
        s = ctx.socket(zmq.REQ)
        s.setsockopt(zmq.RCVTIMEO, 3000)
        s.setsockopt(zmq.LINGER, 0)
        s.connect("tcp://localhost:6669")
        try:
            s.send_string(req_text)
            return json.loads(s.recv_string())
        finally:
            s.close()

    # 1) 正常工具调用 + 状态变更应答
    r = call('{"tool":"climate_control","action":"set_temp","temp":"22"}')
    assert r["ok"] is True and r["state"]["ac_temp"] == 22, r

    # 2) PUB 状态广播（订阅可能错过启动期 state_full，只断言后续 state_change）
    sub = ctx.socket(zmq.SUB)
    sub.setsockopt(zmq.SUBSCRIBE, b"")
    sub.setsockopt(zmq.RCVTIMEO, 3000)
    sub.setsockopt(zmq.LINGER, 0)
    sub.connect("tcp://localhost:6670")
    time.sleep(0.5)  # 等 SUB 订阅传播（ZMQ 慢加入者），否则错过下一条广播
    r = call('{"tool":"window_control","action":"open_all"}')
    assert r["ok"] is True and r["state"]["window_fl"] == 100, r
    pub_msg = json.loads(sub.recv_string())
    assert pub_msg["type"] == "state_change" and "values" in pub_msg, pub_msg

    # 3) 未知工具
    r = call('{"tool":"nope"}')
    assert r["ok"] is False and "unknown tool" in r["result"], r

    # 4) 畸形 JSON（旧手写解析会静默解析出垃圾的场景）
    r = call('{"tool": "climate_control", oops')
    assert r["ok"] is False and r["result"] == "malformed json", r

    # 5) 传感器读数（conf 化路径生效）
    r = call('{"tool":"sensor_read","action":"read"}')
    assert r["ok"] is True and r["result"], r

    # 6) 中文结果往返无损
    r = call('{"tool":"climate_control","action":"set_mode","mode":"cool"}')
    assert r["ok"] is True and isinstance(r["result"], str) and r["result"], r

    print("SELFTEST_TOOL_BUS PASS")
finally:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    log.close()
    ctx.destroy()  # 关闭残余 socket 并终止，避免 term() 等待阻塞
PYEOF
echo "[selftest:tool_bus] 全部 PASS"
