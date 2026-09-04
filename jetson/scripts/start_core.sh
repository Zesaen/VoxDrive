#!/usr/bin/env bash
# start_core.sh — VoxDrive Jetson 侧全栈启动编排（路径无关：以脚本位置定位 jetson/ 根，
# 端口/模型/参数全部读 config/voxdrive.conf）。
# 启动顺序：llama-server → llm → rag → tts(含自检) → tool_bus → intent_router
# 环境变量：VOX_START_DASHBOARD=1 时追加启动 dashboard（需桌面会话）
set -u

JETSON_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONF="$JETSON_DIR/config/voxdrive.conf"

LLAMA_LOG="/tmp/llama-server.log"
LLM_LOG="/tmp/llm.log"
RAG_LOG="/tmp/rag.log"
TTS_LOG="/tmp/tts.log"
TOOL_LOG="/tmp/tool_bus.log"
ROUTER_LOG="/tmp/intent_router.log"
DASH_LOG="/tmp/dashboard.log"
MEDIAMTX_LOG="/tmp/mediamtx.log"
MEDIAMTX_DIR="$(cd "$JETSON_DIR/.." && pwd)/third_party/mediamtx"

# ---- conf 读取（key=value，'#' 注释，$HOME 展开）----
conf_get() {
    awk -F= -v k="$1" '$1 ~ "^"k"[[:space:]]*$" {sub(/#.*/,"",$2); gsub(/^[[:space:]]+|[[:space:]]+$/,"",$2); print $2; exit}' "$CONF" \
        | sed "s|\$HOME|$HOME|g"
}

P_ROUTER=$(conf_get port.intent_router)
P_ROUTER_PUB=$(conf_get port.intent_router_pub)
P_RAG=$(conf_get port.rag)
P_LLM=$(conf_get port.llm)
P_TOOL=$(conf_get port.tool_bus)
P_TTS=$(conf_get port.tts_text)
P_TTS_BLOCK=$(conf_get port.tts_block)
P_TTS_PUB=$(conf_get port.tts_pub)
P_LLAMA=$(conf_get port.llama_http)
LLAMA_BIN=$(conf_get llm.server_bin)
LLM_MODEL=$(conf_get llm.model)
LLM_NGL=$(conf_get llm.ngl)
LLM_CTX=$(conf_get llm.ctx)
TTS_MODEL=$(conf_get model.tts)

wait_for_port() {
    local port="$1" name="$2" retries="${3:-40}"
    while [ "$retries" -gt 0 ]; do
        if ss -tln 2>/dev/null | grep -q ":${port} "; then
            printf '[OK] %s listening on %s\n' "$name" "$port"
            return 0
        fi
        sleep 0.5
        retries=$((retries - 1))
    done
    printf '[FAIL] %s did not bind port %s\n' "$name" "$port"
    return 1
}

kill_port() {
    fuser -k "${1}/tcp" >/dev/null 2>&1 || true
}

kill_existing() {
    for p in "$P_ROUTER" "$P_RAG" "$P_LLM" "$P_TOOL" "$P_TTS" "$P_TTS_BLOCK" "$P_TTS_PUB" "$P_LLAMA"; do
        kill_port "$p"
    done
    # -x 按进程名精确匹配，避免 -f 误伤自身命令行；mediamtx 二进制随 PATH，用 kill_port 即可
    kill_port "$(conf_get mediamtx.rtmp_port)"
    # -x 按进程名精确匹配，避免 -f 误伤自身命令行
    pkill -9 -x intent_router 2>/dev/null || true
    pkill -9 -f '[l]lm_server.py' 2>/dev/null || true
    pkill -9 -f '[r]ag_server.py' 2>/dev/null || true
    pkill -9 -x tts_server 2>/dev/null || true
    pkill -9 -x tool_bus 2>/dev/null || true
    pkill -9 -x llama-server 2>/dev/null || true
    pkill -9 -x mediamtx 2>/dev/null || true
    sleep 1
}

start_mediamtx() {
    if [ ! -x "$MEDIAMTX_DIR/mediamtx" ]; then
        printf '[SKIP] mediamtx 未部署（%s 缺失），预览推拉流不可用\n' "$MEDIAMTX_DIR/mediamtx"
        return 0
    fi
    nohup "$MEDIAMTX_DIR/mediamtx" "$MEDIAMTX_DIR/mediamtx.yml" >"$MEDIAMTX_LOG" 2>&1 </dev/null &
    wait_for_port "$(conf_get mediamtx.rtmp_port)" "mediamtx" 20
}

start_llama() {
    # 大模型加载前清页缓存（无免密 sudo 时跳过；NvMap 碎片化会致 cudaMalloc OOM）
    sudo -n sh -c "echo 3 > /proc/sys/vm/drop_caches" >/dev/null 2>&1 || true
    nohup env GGML_CUDA_NO_VMM=1 "$LLAMA_BIN" \
        -m "$LLM_MODEL" -ngl "$LLM_NGL" -c "$LLM_CTX" \
        --host 127.0.0.1 --port "$P_LLAMA" \
        >"$LLAMA_LOG" 2>&1 </dev/null &
    wait_for_port "$P_LLAMA" "llama-server" 90
}

start_llm() {
    nohup /usr/bin/python3 "$JETSON_DIR/services/llm/llm_server.py" >"$LLM_LOG" 2>&1 </dev/null &
    wait_for_port "$P_LLM" "llm_server"
}

start_rag() {
    nohup /usr/bin/python3 "$JETSON_DIR/services/rag/rag_server.py" >"$RAG_LOG" 2>&1 </dev/null &
    wait_for_port "$P_RAG" "rag_server"
}

start_tts() {
    nohup "$JETSON_DIR/services/tts/build/tts_server" "$TTS_MODEL" >"$TTS_LOG" 2>&1 </dev/null &
    wait_for_port "$P_TTS" "tts_server"
    wait_for_port "$P_TTS_BLOCK" "tts_block"
    wait_for_port "$P_TTS_PUB" "tts_play_end"
    verify_tts
}

verify_tts() {
    /usr/bin/python3 - "$P_TTS_BLOCK" "$P_TTS" <<'PY'
import sys
import time
import zmq

block_port, text_port = sys.argv[1], sys.argv[2]
ctx = zmq.Context()
status = ctx.socket(zmq.REQ)
status.setsockopt(zmq.RCVTIMEO, 5000)
status.setsockopt(zmq.LINGER, 0)
status.connect(f"tcp://127.0.0.1:{block_port}")

text = ctx.socket(zmq.REQ)
text.setsockopt(zmq.RCVTIMEO, 15000)
text.setsockopt(zmq.LINGER, 0)
text.connect(f"tcp://127.0.0.1:{text_port}")

try:
    status.send_string("block")
    if status.recv_string() != "ok":
        raise RuntimeError("tts block handshake failed")
    time.sleep(0.2)
    text.send_string("启动自检语音 END")
    reply = text.recv_string()
    if "Echo" not in reply:
        raise RuntimeError(f"unexpected tts reply: {reply}")
    print("[OK] tts self-check passed")
    sys.exit(0)
except Exception as exc:
    print(f"[FAIL] tts self-check failed: {exc}")
    sys.exit(1)
finally:
    status.close()
    text.close()
    ctx.destroy()
PY
}

start_tool_bus() {
    nohup "$JETSON_DIR/services/tool_bus/build/tool_bus" >"$TOOL_LOG" 2>&1 </dev/null &
    wait_for_port "$P_TOOL" "tool_bus"
}

start_router() {
    nohup "$JETSON_DIR/services/intent_router/build/intent_router" >"$ROUTER_LOG" 2>&1 </dev/null &
    wait_for_port "$P_ROUTER" "intent_router"
}

start_dashboard() {
    nohup /usr/bin/python3 "$JETSON_DIR/dashboard/dashboard_ui.py" >"$DASH_LOG" 2>&1 </dev/null &
    printf '[OK] dashboard started (log %s)\n' "$DASH_LOG"
}

print_status() {
    printf '\n[Ports]\n'
    ss -tlnp 2>/dev/null | grep -E "$P_ROUTER|$P_RAG|$P_LLM|$P_TOOL|$P_TTS|$P_LLAMA|$(conf_get mediamtx.rtmp_port)|$(conf_get mediamtx.rtsp_port)" || true
    printf '\n[Processes]\n'
    pgrep -af 'llama-server|llm_server.py|rag_server.py|tts_server|tool_bus|intent_router|dashboard_ui|mediamtx' || true
    printf '\n[Logs]\n'
    for f in llama llm rag tts tool_bus intent_router dashboard; do
        printf '%-10s /tmp/%s.log\n' "$f" "$f"
    done
}

main() {
    kill_existing
    start_mediamtx || true
    start_llama || exit 1
    start_llm || exit 1
    start_rag || exit 1
    start_tts || exit 1
    start_tool_bus || exit 1
    start_router || exit 1
    [ "${VOX_START_DASHBOARD:-0}" = "1" ] && start_dashboard
    print_status
}

main "$@"
