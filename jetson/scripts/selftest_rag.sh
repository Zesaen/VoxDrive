#!/usr/bin/env bash
# rag 板上自测：拉起 rag_server（加载嵌入模型）→ 3 条查询断言（命中/未命中）→ 清理
# 前置：sync_to_board.sh 已同步；板上 models/embedding 已就位（models_manifest.md）
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC_DIR="$REMOTE_DIR/services/rag"

ssh "$BOARD" "test -d ~/Desktop/VoxDrive/models/embedding" \
  || { echo "[selftest:rag] 缺 models/embedding，先按 models_manifest.md 部署"; exit 1; }

echo "[selftest:rag] 拉起 rag_server 并断言"
ssh "$BOARD" "pkill -f rag_server.py 2>/dev/null || true; sleep 0.3"
ssh "$BOARD" "/usr/bin/python3 -u - '$SVC_DIR/rag_server.py'" <<'PYEOF'
import json
import signal
import subprocess
import sys
import time

import zmq

server_py = sys.argv[1]
log = open("/tmp/rag.log", "w")
proc = subprocess.Popen(["/usr/bin/python3", server_py], stdout=log, stderr=log)
try:
    # 模型加载（CPU 加载 SentenceTransformer）需要时间，轮询端口就绪
    ctx = zmq.Context()
    for _ in range(60):
        time.sleep(1)
        if proc.poll() is not None:
            sys.exit("rag_server 启动即退出，查看 /tmp/rag.log")
        try:
            s = ctx.socket(zmq.REQ)
            s.setsockopt(zmq.RCVTIMEO, 500)
            s.setsockopt(zmq.LINGER, 0)
            s.connect("tcp://localhost:6667")
            s.close()
            break
        except Exception:
            continue

    def ask(q):
        s = ctx.socket(zmq.REQ)
        s.setsockopt(zmq.RCVTIMEO, 15000)
        s.setsockopt(zmq.LINGER, 0)
        s.connect("tcp://localhost:6667")
        try:
            t0 = time.time()
            s.send_string(q)
            r = json.loads(s.recv_string())
            return r, (time.time() - t0) * 1000
        finally:
            s.close()

    # 1) 手册问题应命中
    r, ms = ask("保养周期是多少")
    assert r["found"] is True and r["text"], r
    print(f"  query1 '保养周期' found, {ms:.0f} ms")
    # 2) 另一手册问题
    r, ms = ask("制动系统故障怎么办")
    assert r["found"] is True, r
    print(f"  query2 '制动系统' found, {ms:.0f} ms")
    # 3) 无关问题应未命中（阈值过滤生效）
    r, ms = ask("今天天气怎么样")
    assert r["found"] is False, r
    print(f"  query3 '天气' not-found, {ms:.0f} ms")

    print("SELFTEST_RAG PASS")
finally:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    log.close()
    ctx.destroy()
PYEOF
echo "[selftest:rag] 全部 PASS"
