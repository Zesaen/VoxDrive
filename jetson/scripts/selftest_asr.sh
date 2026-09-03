#!/usr/bin/env bash
# asr 板上自测：编译 → 零参数启动（模型路径自动取 conf）→ 断言模型加载/麦克风打开/识别循环存活 → SIGINT 清理
# 真人发音的声学端到端验证属 A9 联调项，本自测验证到"识别循环稳定运行"层。
# 前置：sync_to_board.sh 已同步；models/asr-zipformer-zh-en 与
#       jetson/services/asr/sherpa-onnx（vendored 树）已板上就位（models_manifest.md）
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC_DIR="$REMOTE_DIR/services/asr"

ssh "$BOARD" "test -d ~/Desktop/VoxDrive/models/asr-zipformer-zh-en && test -d '$SVC_DIR/sherpa-onnx/build/lib'" \
  || { echo "[selftest:asr] 缺模型或 sherpa-onnx 树（见 models_manifest.md，需板上一次性拷贝）"; exit 1; }

echo "[selftest:asr] 编译"
ssh "$BOARD" "set -e
  mkdir -p '$SVC_DIR/build'
  cd '$SVC_DIR/build'
  cmake .. -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build . -j\$(nproc) >/dev/null"

echo "[selftest:asr] 零参数启动并断言存活"
ssh "$BOARD" "pkill -x sherpa-onnx-microphone 2>/dev/null || true; sleep 0.3"
ssh "$BOARD" "/usr/bin/python3 -u - '$SVC_DIR/build/sherpa-onnx-microphone'" <<'PYEOF'
import signal
import subprocess
import sys
import time

bin_path = sys.argv[1]
log = open("/tmp/asr.log", "w")
proc = subprocess.Popen([bin_path], stdout=log, stderr=log)
try:
    deadline = time.time() + 40  # 模型加载（int8 ~200MB）需数秒
    ready = False
    while time.time() < deadline:
        if proc.poll() is not None:
            sys.exit("asr 启动即退出，查看 /tmp/asr.log")
        with open("/tmp/asr.log", errors="ignore") as f:
            content = f.read()
        if "麦克风流已启动" in content:
            ready = True
            break
        if "无默认输入设备" in content or "portaudio error" in content:
            sys.exit("麦克风打开失败，查看 /tmp/asr.log（可用 SHERPA_ONNX_MIC_DEVICE 指定设备号）")
        time.sleep(1)
    if not ready:
        sys.exit("40s 内未进入识别循环，查看 /tmp/asr.log")

    # 识别循环稳定运行 8s
    time.sleep(8)
    assert proc.poll() is None, "识别循环中崩溃，查看 /tmp/asr.log"
    print("SELFTEST_ASR PASS（模型加载+麦克风+识别循环正常；声学端到端留待 A9 真人验证）")
finally:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    log.close()
PYEOF
echo "[selftest:asr] 全部 PASS"
