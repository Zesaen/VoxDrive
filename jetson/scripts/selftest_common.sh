#!/usr/bin/env bash
# A1 公共地基板上自测：C++（vox_config/vox_log/msg_envelope/json.hpp）+ Python（common 包）
# 前置：sync_to_board.sh 已把 jetson/ 同步到板上
# 环境变量: VOX_BOARD(默认 nvidia)、VOX_REMOTE_DIR(默认 ~/Desktop/VoxDrive/jetson)
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"

echo "[selftest:common] C++ 侧"
ssh "$BOARD" "set -e
  cd '$REMOTE_DIR/common/selftest'
  g++ -std=c++17 -Wall -I.. selftest_common.cpp -o /tmp/vox_selftest_common
  /tmp/vox_selftest_common"

echo "[selftest:common] Python 侧"
ssh "$BOARD" "/usr/bin/python3 - '$REMOTE_DIR'" <<'PYEOF'
import sys
root = sys.argv[1]          # ~/Desktop/VoxDrive/jetson
sys.path.insert(0, root)
from common import vox_config, vox_log, msg_envelope

assert vox_config.load()
assert vox_config.get_int("port.tool_bus", 0) == 6669
assert vox_config.bind_endpoint("port.rag", "0") == "tcp://*:6667"
assert vox_config.get("llm.model", "").startswith("/")

env = msg_envelope.make(msg_envelope.TYPE_EVENT, "selftest_py", {"ok": 1})
assert msg_envelope.valid(env) and env["payload"]["ok"] == 1

log = vox_log.setup("selftest_py")
log.info("python 侧配置/信封/日志验证通过")

print("SELFTEST_PY PASS")
PYEOF

echo "[selftest:common] 全部 PASS"
