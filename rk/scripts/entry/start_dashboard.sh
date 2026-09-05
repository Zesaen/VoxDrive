#!/usr/bin/env bash
# RK 触摸屏启动 dashboard（由 sync_to_rk.sh 部署到板根 ~/Desktop/VoxDrive/）
# 用法：
#   ./start_dashboard.sh             # 真屏 X 会话（DISPLAY=:0，GDM 登录后可用）
#   ./start_dashboard.sh --offscreen # 离屏冒烟（无显示环境）
set -u

# 板根定位：仓库内(rk/scripts/entry/)与板上部署(板根)两处都能向上找到 jetson/
D="$(cd "$(dirname "$0")" && pwd)"
BOARD_ROOT=""
for c in "$D" "$D/.." "$D/../.." "$D/../../.."; do
  if [ -f "$(cd "$c" 2>/dev/null && pwd)/jetson/dashboard/dashboard_ui.py" ]; then
    BOARD_ROOT="$(cd "$c" && pwd)"
    break
  fi
done
[ -n "$BOARD_ROOT" ] || { echo "错误: 从 $D 向上找不到 jetson/dashboard"; exit 1; }
export VOX_CONF="$BOARD_ROOT/jetson/config/voxdrive.rk.conf"
# 显示探测：Xorg 由 displayfd 启动时显示号不定（本板为 :1），从 X socket 自动取
if [ -z "${DISPLAY:-}" ]; then
  XN=$(ls /tmp/.X11-unix/ 2>/dev/null | sed -n 's/^X//p' | sort -n | head -1)
  export DISPLAY=":${XN:-0}"
fi
if [ -z "${XAUTHORITY:-}" ] && [ -f "/run/user/$(id -u)/gdm/Xauthority" ]; then
  export XAUTHORITY="/run/user/$(id -u)/gdm/Xauthority"
fi
case "${1:-}" in
  --offscreen) export QT_QPA_PLATFORM=offscreen ;;
  "") [ -n "${QT_QPA_PLATFORM:-}" ] || export QT_QPA_PLATFORM=xcb ;;
  *) echo "用法: $0 [--offscreen]"; exit 2 ;;
esac

echo "[dash] conf=$VOX_CONF platform=$QT_QPA_PLATFORM display=$DISPLAY"
exec python3 "$BOARD_ROOT/jetson/dashboard/dashboard_ui.py" "$@"
