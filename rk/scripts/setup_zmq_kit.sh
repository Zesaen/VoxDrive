#!/usr/bin/env bash
# 拷贝 zmq-comm-kit 源码到 RK 板并编译（C5 前置，在 PC 上执行）。
# 规则：资料一律直接复制、禁止软链接；只送源码不含 build 产物。
# 用法：./setup_zmq_kit.sh [kit源码目录]   # 缺省取环境变量 VOX_KIT_SRC
set -euo pipefail

BOARD="${VOX_RK_BOARD:-lubancat}"
REMOTE_DIR='$HOME/Desktop/VoxDrive/zmq-comm-kit'
KIT_SRC="${1:-${VOX_KIT_SRC:-}}"

if [ -z "$KIT_SRC" ]; then
  echo "用法: $0 <zmq-comm-kit 源码目录>（或设 VOX_KIT_SRC）"
  echo "本地缺省位置: ../../03CarAiassistant/vehicle_llm_voice_agent/zmq-comm-kit"
  exit 1
fi
KIT_SRC="$(cd "$KIT_SRC" && pwd)"

echo "copy $KIT_SRC -> $BOARD:$REMOTE_DIR"
tar -C "$KIT_SRC" --exclude=build -cf - . | ssh "$BOARD" "mkdir -p $REMOTE_DIR && tar -xf - -C $REMOTE_DIR"
ssh "$BOARD" "cd $REMOTE_DIR && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j\$(nproc) 2>&1 | tail -2"
ssh "$BOARD" "ls $REMOTE_DIR/build/libzmq_component.so" && echo "kit ready on RK"
