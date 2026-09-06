#!/usr/bin/env bash
# 增量送 RK 板：git archive 已提交内容 → 板上 ~/VoxDrive（目录结构与仓库镜像）
# 只送已 commit 的内容，保证板上运行的就是仓库里的代码。
# 用法：./sync_to_rk.sh                    # 默认 rk + jetson/common + jetson/config
#       ./sync_to_rk.sh <git pathspec>     # 自定义（如只送 rk/capture）
set -euo pipefail

BOARD="${VOX_RK_BOARD:-lubancat}"
REMOTE_DIR="${VOX_RK_DIR:-\$HOME/Desktop/VoxDrive}"
PATHSPEC="${1:-rk jetson/common jetson/config jetson/dashboard jetson/services/tts}"

# 仓库根（脚本位于 rk/scripts/）
cd "$(dirname "$0")/../.."

if [ -n "$(git status --porcelain -- rk jetson/common jetson/config jetson/dashboard jetson/services/tts)" ]; then
  echo "警告：rk/ 或 jetson/* 有未提交改动，本次只送已提交内容"
fi

# PC 侧脚本不上板（在仓库/PC 上运行、经 ssh 操作板子）
EXCLUDES=( ':(exclude)rk/scripts/sync_to_rk.sh' ':(exclude)rk/scripts/build_on_rk.sh' )

ssh "$BOARD" "mkdir -p $REMOTE_DIR"
git archive HEAD $PATHSPEC "${EXCLUDES[@]}" | ssh "$BOARD" "tar -x -C $REMOTE_DIR"

# 板根目录部署顶层入口；清掉历史同步残留的 PC 侧脚本
ssh "$BOARD" "rm -f $REMOTE_DIR/rk/scripts/sync_to_rk.sh $REMOTE_DIR/rk/scripts/build_on_rk.sh
  for f in $REMOTE_DIR/rk/scripts/entry/*.sh; do
    cp -f \"\$f\" $REMOTE_DIR/
    chmod +x $REMOTE_DIR/\$(basename \"\$f\")
  done
  echo '[entry] 板根目录入口:' && ls $REMOTE_DIR/*.sh"
echo "synced [$PATHSPEC] -> $BOARD:Desktop/VoxDrive"
