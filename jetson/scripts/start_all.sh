#!/usr/bin/env bash
# start_all.sh — VoxDrive 双板一键启动（在 Jetson 上运行）
#
# 编排顺序：
#   ① RK recorder_service：先 ZMQ 探测（在跑则跳过），不在则经 SSH 免密拉起并复测
#   ② Jetson 全栈：mediamtx + llama-server + llm/rag/tts/tool_bus/intent_router（复用 start_core.sh）
#   ③ 预览推流：默认打开 RK 推流（mediamtx 即有流，dashboard/HLS 立刻有画面）
#
# 用法：
#   ./start_all.sh                 # 默认开预览
#   ./start_all.sh --no-preview    # 不推流（仅录像+语音链路）
#   VOX_START_DASHBOARD=1 ./start_all.sh   # Jetson 有桌面会话时同时启动 dashboard GUI
#
# 依赖（一次性）：Jetson→cat@rk.ip 的 SSH 免密（公钥装入 RK authorized_keys，
# 配置方法见 newPrj/AGENTS.md 组网节）。脚本内不存任何口令。
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF="$SCRIPT_DIR/../config/voxdrive.conf"
RK_IP=$(awk -F= '$1 ~ /^rk\.ip[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
RK_USER=$(awk -F= '$1 ~ /^rk\.ssh_user[[:space:]]*$/ {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF")
RK_USER="${RK_USER:-cat}"
PREVIEW=1
[ "${1:-}" = "--no-preview" ] && PREVIEW=0

rk_status() {  # 0=在跑 1=无应答；直发消息信封 status 命令
    /usr/bin/python3 - "$RK_IP" <<'PY' >/dev/null 2>&1
import json, sys, time, zmq
ctx = zmq.Context()
s = ctx.socket(zmq.REQ); s.setsockopt(zmq.RCVTIMEO, 3000); s.setsockopt(zmq.LINGER, 0)
s.connect("tcp://%s:6700" % sys.argv[1])
s.send_string(json.dumps({"version": 1, "type": "status", "timestamp_ms": int(time.time()*1000),
                          "source": "jetson.start_all", "payload": {"cmd": "status"}}))
s.recv_string()
ctx.destroy()
PY
}

rk_req() {  # rk_req <value json bool>：发 set_preview
    /usr/bin/python3 - "$RK_IP" "$1" <<'PY' >/dev/null 2>&1
import json, sys, time, zmq
ctx = zmq.Context()
s = ctx.socket(zmq.REQ); s.setsockopt(zmq.RCVTIMEO, 3000); s.setsockopt(zmq.LINGER, 0)
s.connect("tcp://%s:6700" % sys.argv[1])
s.send_string(json.dumps({"version": 1, "type": "status", "timestamp_ms": int(time.time()*1000),
                          "source": "jetson.start_all", "payload": {"cmd": "set_preview", "value": sys.argv[2] == "1"}}))
s.recv_string()
ctx.destroy()
PY
}

# ── ① RK recorder_service ──
if rk_status; then
    printf '[OK] RK recorder_service 已在运行（%s）\n' "$RK_IP"
else
    printf '[..] RK 服务未运行，SSH 拉起 %s@%s\n' "$RK_USER" "$RK_IP"
    if ssh -o BatchMode=yes -o ConnectTimeout=6 "$RK_USER@$RK_IP" \
        "pkill -9 -x recorder_servic 2>/dev/null; sleep 1; \
         (nohup ~/Desktop/VoxDrive/rk/build/recorder_service > /tmp/recorder_service.log 2>&1 &)" 2>/dev/null
    then
        sleep 4
        if rk_status; then
            printf '[OK] RK recorder_service 拉起成功\n'
        else
            printf '[WARN] RK 已执行启动命令但状态无应答（日志：RK /tmp/recorder_service.log）\n'
        fi
    else
        printf '[WARN] RK SSH 不可达（%s@%s）——继续启动 Jetson 侧，行车记录功能不可用\n' "$RK_USER" "$RK_IP"
    fi
fi

# ── ② Jetson 全栈（含 mediamtx）──
"$SCRIPT_DIR/start_core.sh" || exit 1

# ── ③ 预览推流 ──
if [ "$PREVIEW" = "1" ]; then
    if rk_req 1; then
        printf '[OK] RK 预览推流已开启 → mediamtx（RTSP 8554 / HLS 8888 / RTMP 1935）\n'
    else
        printf '[WARN] 预览开启命令无应答（录像不受影响）\n'
    fi
fi

printf '\n====== 就绪 ======\n'
printf '键盘语音测试：python3 %s/services/asr/stdin_asr.py\n' "$SCRIPT_DIR/.."
printf '浏览器看画面：http://%s:8888/live/dashcam\n' "$(hostname -I | awk '{print $1}')"
printf '试试输入：现在录着吗 / 行车记录仪还剩多少存储 / 帮我拍张照 / 停止录像 / 开始录像 / 关闭预览\n'
