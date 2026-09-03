#!/usr/bin/env bash
# tts 板上自测：编译 → 拉起 tts_server → 三端口握手断言（6677 block→ok、7777 文本→Echo、
# 6678 play_end）→ 清理。无扬声器时播放跳过但 play_end 仍发布（协议完整性可测）。
# 前置：sync_to_board.sh 已同步；models/single_speaker_fast.bin 与
#       jetson/services/tts/eigen-3.4.0 已板上就位（models_manifest.md）
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC_DIR="$REMOTE_DIR/services/tts"

ssh "$BOARD" "test -f ~/Desktop/VoxDrive/models/single_speaker_fast.bin && test -d '$SVC_DIR/eigen-3.4.0'" \
  || { echo "[selftest:tts] 缺模型或 eigen（见 models_manifest.md，需板上一次性拷贝）"; exit 1; }

echo "[selftest:tts] 编译（vendored 引擎约 1-2 分钟）"
ssh "$BOARD" "set -e
  mkdir -p '$SVC_DIR/build'
  cd '$SVC_DIR/build'
  cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build . -j\$(nproc) >/dev/null"

echo "[selftest:tts] 拉起并断言三端口握手"
ssh "$BOARD" "pkill -x tts_server 2>/dev/null || true; sleep 0.3"
ssh "$BOARD" "/usr/bin/python3 -u - '$SVC_DIR/build/tts_server'" <<'PYEOF'
import signal
import subprocess
import sys
import time

import zmq

bin_path = sys.argv[1]
log = open("/tmp/tts.log", "w")
proc = subprocess.Popen([bin_path], stdout=log, stderr=log)
try:
    time.sleep(3)  # 模型加载
    if proc.poll() is not None:
        sys.exit("tts_server 启动即退出，查看 /tmp/tts.log")

    ctx = zmq.Context()
    # play_end 订阅先行（慢加入者）
    sub = ctx.socket(zmq.SUB)
    sub.setsockopt(zmq.SUBSCRIBE, b"")
    sub.setsockopt(zmq.RCVTIMEO, 60000)   # CPU 合成 + ALSA 播放可能较慢
    sub.setsockopt(zmq.LINGER, 0)
    sub.connect("tcp://localhost:6678")
    time.sleep(0.5)

    def req(port, text, timeout_ms=10000):
        s = ctx.socket(zmq.REQ)
        s.setsockopt(zmq.RCVTIMEO, timeout_ms)
        s.setsockopt(zmq.LINGER, 0)
        s.connect(f"tcp://localhost:{port}")
        try:
            s.send_string(text)
            return s.recv_string()
        finally:
            s.close()

    # 1) 阻塞门握手
    r = req(6677, "block")
    assert r == "ok", r
    # 2) 播报文本（" END" 结束标记）
    t0 = time.time()
    r = req(7777, "启动自检测试 END")
    assert "Echo" in r, r
    # 3) play_end 事件
    ev = sub.recv_string()
    assert ev == "play_end", ev
    print(f"  三端口握手 ok，播报端到端 {time.time()-t0:.1f}s（含合成+播放）")

    print("SELFTEST_TTS PASS")
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
echo "[selftest:tts] 全部 PASS"
