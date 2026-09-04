#!/usr/bin/env bash
# stop_all.sh — 双板一键停止（与 start_all.sh 对应；在 Jetson 上运行）
# 停 Jetson 全栈（含 mediamtx/dashboard）+ RK recorder_service（SSH 免密）。
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$SCRIPT_DIR/../config/voxdrive.conf"
RK_IP=$(awk -F= '$1 ~ /^rk\.ip[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
RK_USER=$(awk -F= '$1 ~ /^rk\.ssh_user[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
RK_USER="${RK_USER:-cat}"

# Jetson 侧：进程名精确匹配（pkill -x），dashboard 用括号防自匹配
pkill -9 -f '[d]ashboard_ui' 2>/dev/null || true
for p in mediamtx intent_router tool_bus tts_server llama-server; do
    pkill -9 -x "$p" 2>/dev/null || true
done
pkill -9 -f '[l]lm_server.py' 2>/dev/null || true
pkill -9 -f '[r]ag_server.py' 2>/dev/null || true
printf '[OK] Jetson 侧已停止\n'

# RK 侧：进程名 15 字符截断（recorder_servic）
if ssh -o BatchMode=yes -o ConnectTimeout=6 "$RK_USER@$RK_IP" \
    "pkill -9 -x recorder_servic 2>/dev/null" 2>/dev/null
then
    printf '[OK] RK recorder_service 已停止\n'
else
    printf '[WARN] RK SSH 不可达，跳过远端停止\n'
fi
