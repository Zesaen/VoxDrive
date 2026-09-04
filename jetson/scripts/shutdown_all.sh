#!/usr/bin/env bash
# shutdown_all.sh — VoxDrive 双板一键安全关机（在 Jetson 上运行）
#
# 顺序：
#   ① 优雅停服务：RK 先 SIGTERM（recorder_service 收好当前 MP4 段）再 SIGKILL 兜底；
#      Jetson 全栈同理
#   ② RK 关机：SSH 免密 + sudoers 定向 NOPASSWD（仅放行 poweroff/reboot，一次性配置，
#      见 newPrj/AGENTS.md 组网节；脚本内不存任何口令）
#   ③ 本机关机：sudo poweroff——Jetson 未配免密 sudo，按提示输入一次密码；
#      倒计时 5 秒内 Ctrl+C 可取消
#
# 用法：
#   ./shutdown_all.sh             # 全流程：停服务 → 关 RK → 关本机
#   ./shutdown_all.sh --no-self   # 停本机服务 + 关 RK，本机不关机
#   ./shutdown_all.sh --dry-run   # 只打印将执行的动作，不停不停电
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$SCRIPT_DIR/../config/voxdrive.conf"
RK_IP=$(awk -F= '$1 ~ /^rk\.ip[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
RK_USER=$(awk -F= '$1 ~ /^rk\.ssh_user[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
RK_USER="${RK_USER:-cat}"
SELF=1
DRY=0
for a in "$@"; do
    case "$a" in
        --no-self) SELF=0 ;;
        --dry-run) DRY=1 ;;
    esac
done

JETSON_PROCS="mediamtx intent_router tool_bus tts_server llama-server"

# ── ① 停服务（优雅 TERM → 3s 后强杀兜底）──
if [ "$DRY" = "1" ]; then
    printf '[dry-run] 将执行：RK(%s@%s) 优雅停 recorder_service → Jetson 停 %s\n' \
        "$RK_USER" "$RK_IP" "$JETSON_PROCS"
    printf '[dry-run] 将执行：ssh %s@%s sudo -n /usr/sbin/poweroff\n' "$RK_USER" "$RK_IP"
    [ "$SELF" = "1" ] && echo '[dry-run] 将执行：本机 5s 倒计时后 sudo poweroff'
    exit 0
fi

printf '[..] RK 优雅停止（SIGTERM，收好当前录像段）\n'
ssh -o BatchMode=yes -o ConnectTimeout=6 "$RK_USER@$RK_IP" \
    "pkill -x recorder_servic 2>/dev/null" 2>/dev/null || true
sleep 2
ssh -o BatchMode=yes -o ConnectTimeout=6 "$RK_USER@$RK_IP" \
    "pkill -9 -x recorder_servic 2>/dev/null" 2>/dev/null || true

pkill -f '[d]ashboard_ui' 2>/dev/null || true
for p in $JETSON_PROCS; do pkill -x "$p" 2>/dev/null || true; done
pkill -f '[l]lm_server.py' 2>/dev/null || true
pkill -f '[r]ag_server.py' 2>/dev/null || true
sleep 3
for p in $JETSON_PROCS; do pkill -9 -x "$p" 2>/dev/null || true; done
printf '[OK] 双板服务已停止\n'

# ── ② RK 关机 ──
if ssh -o BatchMode=yes -o ConnectTimeout=6 "$RK_USER@$RK_IP" \
    "sudo -n /usr/sbin/poweroff" 2>/dev/null
then
    printf '[OK] RK（%s）关机指令已发出\n' "$RK_IP"
else
    printf '[WARN] RK 关机失败（SSH 不通或 sudoers 未配置）——继续本机流程\n'
fi

# ── ③ 本机关机 ──
if [ "$SELF" = "1" ]; then
    printf '\n本机将在 5 秒后关机（Ctrl+C 取消），届时按提示输入一次 sudo 密码：\n'
    for i in 5 4 3 2 1; do printf '  %d...\n' "$i"; sleep 1; done
    exec sudo poweroff
else
    printf '[OK] 本机仅停服务（--no-self，不关机）\n'
fi
