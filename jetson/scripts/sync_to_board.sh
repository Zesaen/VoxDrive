#!/usr/bin/env bash
# 增量送板：把已提交(git HEAD)的 jetson/ 子树同步到板上 ~/Desktop/VoxDrive/jetson/
# 用法:
#   ./sync_to_board.sh            # 同步整个 jetson/
#   ./sync_to_board.sh tool_bus   # 只同步 jetson/services/tool_bus
# 环境变量: VOX_BOARD(默认 nvidia)、VOX_REMOTE_DIR(默认 ~/Desktop/VoxDrive/jetson)
# 注意: 只送"已提交"内容——先 commit 再 sync，保证板上与仓库一致。
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-~/Desktop/VoxDrive/jetson}"
REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TARGET="${1:-all}"

if [ "$TARGET" = "all" ]; then
  PATHSPEC="jetson"
else
  PATHSPEC="jetson/services/$TARGET"
  # 校验该路径存在于 HEAD，避免 archive 静默输出空包
  git -C "$REPO_ROOT" cat-file -e "HEAD:$PATHSPEC" 2>/dev/null \
    || { echo "[sync] 错误: HEAD 中不存在 $PATHSPEC（先 commit，或检查服务名）"; exit 1; }
fi

echo "[sync] git archive HEAD:$PATHSPEC -> $BOARD:$REMOTE_DIR"
ssh "$BOARD" "mkdir -p $REMOTE_DIR"
# --strip-components=1 去掉顶层 jetson/ 前缀，落点即远端 jetson 根
git -C "$REPO_ROOT" archive HEAD "$PATHSPEC" \
  | ssh "$BOARD" "tar -x -C $REMOTE_DIR --strip-components=1"
echo "[sync] 完成"
