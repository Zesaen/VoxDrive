#!/usr/bin/env bash
# RK 触摸屏启动 dashboard（由 sync_to_rk.sh 部署到板根 ~/Desktop/VoxDrive/）
# 用法：
#   ./start_dashboard.sh             # 真屏 X 会话（DISPLAY=:0，GDM 登录后可用）
#   ./start_dashboard.sh --offscreen # 离屏冒烟（无显示环境）
set -u

BOARD_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
export VOX_CONF="$BOARD_ROOT/jetson/config/voxdrive.rk.conf"
export DISPLAY="${DISPLAY:-:0}"
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
