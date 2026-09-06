#!/usr/bin/env bash
# start_voice — RK 语音推理服务入口（R14：ASR/TTS 推理下沉 RK NPU）
# 板根顶层入口：~/Desktop/VoxDrive/start_voice.sh（sync_to_rk.sh 自动部署）
# 用法: ./start_voice.sh [--stop]
#   启动: nohup voice_service.py，日志 ~/voxdrive_voice.log，PID 落 /tmp/voxdrive_voice.pid
#   依赖: rknnlite(已装 ~/.local) + kaldi-native-fbank + onnxruntime + jieba + pyzmq
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG="$HOME/voxdrive_voice.log"
PIDFILE="/tmp/voxdrive_voice.pid"

if [ "$1" = "--stop" ]; then
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
        kill "$(cat "$PIDFILE")" && echo "voice_service 已停止 (pid $(cat "$PIDFILE"))"
    else
        pkill -f "[v]oice_service.py" && echo "voice_service 已停止 (pkill)" || echo "未在运行"
    fi
    rm -f "$PIDFILE"
    exit 0
fi

if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "voice_service 已在运行 (pid $(cat "$PIDFILE"))，如需重启先 --stop"
    exit 0
fi

cd "$ROOT"
nohup python3 rk/voice/voice_service.py >> "$LOG" 2>&1 &
echo $! > "$PIDFILE"
sleep 2
if kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    echo "voice_service 已启动 (pid $(cat "$PIDFILE"))，日志 $LOG"
    tail -3 "$LOG" || true
else
    echo "启动失败，日志尾部："
    tail -15 "$LOG" || true
    rm -f "$PIDFILE"
    exit 1
fi
