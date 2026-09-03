#!/usr/bin/env bash
# 增量送 RK 板：git archive 已提交内容 → 板上 ~/VoxDrive（目录结构与仓库镜像）
# 只送已 commit 的内容，保证板上运行的就是仓库里的代码。
# 用法：./sync_to_rk.sh                    # 默认 rk + jetson/common + jetson/config
#       ./sync_to_rk.sh <git pathspec>     # 自定义（如只送 rk/capture）
set -euo pipefail

BOARD="${VOX_RK_BOARD:-lubancat}"
REMOTE_DIR="${VOX_RK_DIR:-\$HOME/Desktop/VoxDrive}"
PATHSPEC="${1:-rk jetson/common jetson/config}"

# 仓库根（脚本位于 rk/scripts/）
cd "$(dirname "$0")/../.."

if [ -n "$(git status --porcelain -- rk jetson/common jetson/config)" ]; then
  echo "警告：rk/ 或 jetson/common|config 有未提交改动，本次只送已提交内容"
fi

ssh "$BOARD" "mkdir -p $REMOTE_DIR"
git archive HEAD $PATHSPEC | ssh "$BOARD" "tar -x -C $REMOTE_DIR"
echo "synced [$PATHSPEC] -> $BOARD:Desktop/VoxDrive"
