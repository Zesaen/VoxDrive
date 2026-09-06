#!/usr/bin/env bash
# 板上编译单服务(SSH 远程原生编译): ./build_on_board.sh <service>
# service ∈ tool_bus | intent_router | tts
# 环境变量: VOX_BOARD(默认 nvidia)、VOX_REMOTE_DIR(默认 ~/Desktop/VoxDrive/jetson)
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC="${1:?用法: build_on_board.sh <tool_bus|intent_router|tts>}"

echo "[build] $BOARD:$REMOTE_DIR/services/$SVC"
ssh "$BOARD" "set -e
  mkdir -p '$REMOTE_DIR/services/$SVC/build'
  cd '$REMOTE_DIR/services/$SVC/build'
  cmake .. -DCMAKE_BUILD_TYPE=Release
  cmake --build . -j\$(nproc)"
echo "[build] 完成: $SVC"
