#!/usr/bin/env bash
# RK 板上编译（板端编译定稿）：cmake + make
# 用法：./build_on_rk.sh [target]    # target: voice(rk 主工程) | tts(R14 TTS 节点)，缺省 voice
set -euo pipefail

BOARD="${VOX_RK_BOARD:-lubancat}"
TARGET="${1:-voice}"

case "$TARGET" in
  voice)
    REMOTE_DIR="\$HOME/Desktop/VoxDrive/rk"
    ;;
  tts)
    REMOTE_DIR="\$HOME/Desktop/VoxDrive/rk/tts"
    ;;
  *)
    echo "用法: $0 [voice|tts]" >&2; exit 2
    ;;
esac

ssh "$BOARD" "cd $REMOTE_DIR && \
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build -j\$(nproc)"
echo "build done: $BOARD:$REMOTE_DIR/build"
