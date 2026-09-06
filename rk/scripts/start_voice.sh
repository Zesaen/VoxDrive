#!/usr/bin/env bash
# start_voice — RK 语音推理服务入口（R14：ASR/TTS 推理下沉 RK）
# 板根顶层入口：~/Desktop/VoxDrive/start_voice.sh（sync_to_rk.sh 自动部署）
# 用法: ./start_voice.sh [--stop]
#   启动: voice_service.py（ASR，NPU 三模型）+ tts_node（TTS，SummerTTS CPU 引擎）
#   日志: ~/voxdrive_voice.log（ASR）/ ~/voxdrive_tts.log（TTS）
#   PID:  /tmp/voxdrive_voice.pid / /tmp/voxdrive_tts.pid
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CONF="$ROOT/jetson/config/voxdrive.rk.conf"
VLOG="$HOME/voxdrive_voice.log"
TLOG="$HOME/voxdrive_tts.log"
VPID="/tmp/voxdrive_voice.pid"
TPID="/tmp/voxdrive_tts.pid"

stop_one() {  # $1=pidfile $2=pkill 模式
    if [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; then
        kill "$(cat "$1")" && echo "已停止 ($(cat $1))"
    else
        pkill -f "$2" && echo "已停止 (pkill $2)" || true
    fi
    rm -f "$1"
}

if [ "$1" = "--stop" ]; then
    stop_one "$VPID" "[v]oice_service.py"
    stop_one "$TPID" "[t]ts_node"
    exit 0
fi

# ASR：voice_service.py
if [ -f "$VPID" ] && kill -0 "$(cat "$VPID")" 2>/dev/null; then
    echo "voice_service 已在运行 (pid $(cat "$VPID"))"
else
    cd "$ROOT"
    nohup python3 rk/voice/voice_service.py >> "$VLOG" 2>&1 &
    echo $! > "$VPID"
    sleep 2
    kill -0 "$(cat "$VPID")" 2>/dev/null || { echo "voice_service 启动失败"; tail -5 "$VLOG"; rm -f "$VPID"; exit 1; }
    echo "voice_service 已启动 (pid $(cat "$VPID"))，日志 $VLOG"
fi

# TTS：tts_node（SummerTTS）
if [ -f "$TPID" ] && kill -0 "$(cat "$TPID")" 2>/dev/null; then
    echo "tts_node 已在运行 (pid $(cat "$TPID"))"
else
    if [ ! -x "$ROOT/rk/tts/build/tts_node" ]; then
        echo "[WARN] tts_node 未编译（rk/tts/build/tts_node 缺失），TTS 下沉不可用，仅启动 ASR"
        exit 0
    fi
    nohup "$ROOT/rk/tts/build/tts_node" "$CONF" >> "$TLOG" 2>&1 &
    echo $! > "$TPID"
    sleep 2
    kill -0 "$(cat "$TPID")" 2>/dev/null || { echo "tts_node 启动失败"; tail -5 "$TLOG"; rm -f "$TPID"; exit 1; }
    echo "tts_node 已启动 (pid $(cat "$TPID"))，日志 $TLOG"
fi
