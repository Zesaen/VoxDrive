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
  --no-preview) [ -n "${QT_QPA_PLATFORM:-}" ] || export QT_QPA_PLATFORM=xcb ;;
  "")  [ -n "${QT_QPA_PLATFORM:-}" ] || export QT_QPA_PLATFORM=xcb
       export VOX_DASH_AUTOPREVIEW=1 ;;   # 触摸屏 kiosk 形态默认自动开预览
  *) echo "用法: $0 [--offscreen|--no-preview]"; exit 2 ;;
esac

if [ "$QT_QPA_PLATFORM" != "offscreen" ]; then
  # X 级缩放：dashboard 按 1600x900 设计，物理屏仅 1024x600——把桌面逻辑分辨率放大到
  # 1600x938（1.5625 倍）由 GPU 降采样显示，UI 恢复设计比例。注：QT_SCALE_FACTOR 在本板
  # Qt/驱动组合下首帧后冻结（文字不渲染），故不用 Qt 级缩放；VOX_DASH_SCALE 可显式覆盖。
  XA="${XAUTHORITY:-/run/user/$(id -u)/gdm/Xauthority}"
  OUT=$(DISPLAY="$DISPLAY" XAUTHORITY="$XA" xrandr 2>/dev/null | grep ' connected' | head -1 | cut -d' ' -f1)
  if [ -n "$OUT" ] && [ -z "${VOX_DASH_NO_XSCALE:-}" ]; then
    DISPLAY="$DISPLAY" XAUTHORITY="$XA" xrandr --output "$OUT" --scale 1.5625x1.5625 2>/dev/null \
      || echo "[dash] xrandr 缩放失败（沿用物理分辨率）"
  fi
fi
[ -n "${VOX_DASH_SCALE:-}" ] && export QT_SCALE_FACTOR="$VOX_DASH_SCALE"

echo "[dash] conf=$VOX_CONF platform=$QT_QPA_PLATFORM display=$DISPLAY"
exec python3 "$BOARD_ROOT/jetson/dashboard/dashboard_ui.py" "$@"
