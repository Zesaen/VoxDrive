#!/usr/bin/env bash
# RK 板上编译（板端编译定稿）：cmake + make
# 用法：./build_on_rk.sh          # 全量构建
set -euo pipefail

BOARD="${VOX_RK_BOARD:-lubancat}"
REMOTE_DIR="${VOX_RK_DIR:-\$HOME/Desktop/VoxDrive/rk}"

ssh "$BOARD" "cd $REMOTE_DIR && \
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build -j\$(nproc)"
echo "build done: $BOARD:Desktop/VoxDrive/rk/build"
