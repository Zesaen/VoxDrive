#!/usr/bin/env bash
# dashboard 板上冒烟自测：offscreen 模式拉起 → 注入状态广播（6670/6671）+ 语音事件
# （asr_final/nav，UI v2.0 语音上屏契约）→ 存活与导航断言 → 清理
# 前置：sync_to_board.sh 已同步
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"

echo "[selftest:dashboard] offscreen 冒烟（含语音事件注入）"
ssh "$BOARD" "/usr/bin/python3 -u - '$REMOTE_DIR/dashboard/dashboard_ui.py'" <<'PYEOF'
import json
import os
import signal
import subprocess
import sys
import time

import zmq

ui_py = sys.argv[1]
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", VOX_DASH_REDUCED_MOTION="1")
log = open("/tmp/dashboard.log", "w")
proc = subprocess.Popen(["/usr/bin/python3", ui_py], env=env, stdout=log, stderr=log)
try:
    time.sleep(4)
    if proc.poll() is not None:
        log.close()
        sys.exit("dashboard 启动即退出：\n" + open("/tmp/dashboard.log").read()[-2000:])

    # 注入车控状态 / 服务状态 / 语音事件（asr_final + nav → 应切页不崩）
    ctx = zmq.Context()
    pub_state = ctx.socket(zmq.PUB)
    pub_state.bind("tcp://*:6670")
    pub_status = ctx.socket(zmq.PUB)
    pub_status.bind("tcp://*:6671")
    time.sleep(0.5)  # 等订阅传播
    pub_state.send_string(json.dumps(
        {"type": "state_change", "values": {"ac_temp": "26", "camera_recording": "true"}},
        ensure_ascii=False))
    pub_status.send_string(json.dumps(
        {"service": "rag", "status": "selftest"}, ensure_ascii=False))
    pub_status.send_string(json.dumps(
        {"service": "router", "status": "asr_final", "asr_text": "打开车控"},
        ensure_ascii=False))
    pub_status.send_string(json.dumps(
        {"service": "router", "status": "nav", "target": "vehicle"},
        ensure_ascii=False))
    time.sleep(3)

    assert proc.poll() is None, "dashboard 处理广播时崩溃，查看 /tmp/dashboard.log"
    print("SELFTEST_DASHBOARD PASS")
finally:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    log.close()
    ctx.destroy()
PYEOF

echo "[selftest:dashboard] 语音事件日志断言"
ssh "$BOARD" "grep -c 'voice. nav -> vehicle' /tmp/dashboard.log | grep -q '^1$' \
  && echo 'NAV_LOG PASS' || { echo 'NAV_LOG FAIL'; tail -20 /tmp/dashboard.log; exit 1; }"
echo "[selftest:dashboard] 全部 PASS"
