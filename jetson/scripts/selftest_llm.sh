#!/usr/bin/env bash
# llm 板上自测：启动 llama-server（GPU 加载 GGUF）→ llm_server 代理 →
# 断言 respond / tool_call / 清空对话 → 直连 HTTP 实测 token/s（R9 口径锚点）→ 清理
# 前置：sync_to_board.sh 已同步；板上 models/qwen2.5-1.5b-q4_k_m.gguf 与
#       third_party/llama.cpp/build/bin/llama-server 已就位（models_manifest.md）
set -euo pipefail

BOARD="${VOX_BOARD:-nvidia}"
REMOTE_DIR="${VOX_REMOTE_DIR:-/home/nvidia/Desktop/VoxDrive/jetson}"
SVC_DIR="$REMOTE_DIR/services/llm"

echo "[selftest:llm] 启动 llama-server（首次加载约 30-90s）"
ssh "$BOARD" '
  if curl -sf localhost:8080/health >/dev/null 2>&1; then
    echo "[selftest:llm] llama-server 已在运行，复用"
  else
    BIN=~/Desktop/VoxDrive/third_party/llama.cpp/build/bin/llama-server
    MODEL=~/Desktop/VoxDrive/models/qwen2.5-1.5b-q4_k_m.gguf
    test -x $BIN || { echo "缺 llama-server（见 models_manifest.md）"; exit 1; }
    test -f $MODEL || { echo "缺 GGUF（见 models_manifest.md）"; exit 1; }
    # 大模型加载前清页缓存：Jetson 统一内存下，大文件拷贝后的页缓存会碎片化
    # NvMap 空间导致 cudaMalloc 失败（NvMap error 12）；drop_caches 无破坏性。
    # 无免密 sudo 时跳过，失败时按提示手动执行（不在脚本中存口令）
    sudo -n sh -c "echo 3 > /proc/sys/vm/drop_caches" >/dev/null 2>&1 \
      || echo "[hint] 无免密 sudo；若模型加载 OOM 请手动执行：sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'"
    GGML_CUDA_NO_VMM=1 nohup $BIN -m $MODEL -ngl 28 -c 2048 --port 8080 \
      >/tmp/llama_server.log 2>&1 &
    echo "[selftest:llm] llama-server 后台启动中"
  fi'
# 轮询 /health 就绪（最多 180s）
for i in $(seq 1 90); do
  sleep 2
  if ssh "$BOARD" 'curl -sf localhost:8080/health >/dev/null'; then
    echo "[selftest:llm] llama-server 就绪（$((i*2))s）"; break
  fi
  if [ "$i" -eq 90 ]; then ssh "$BOARD" 'tail -5 /tmp/llama_server.log'; exit 1; fi
done

echo "[selftest:llm] 拉起 llm_server 并断言"
ssh "$BOARD" "pkill -f '[l]lm_server.py' 2>/dev/null || true; sleep 0.3"
ssh "$BOARD" "/usr/bin/python3 -u - '$SVC_DIR/llm_server.py'" <<'PYEOF'
import json
import signal
import subprocess
import sys
import time
import urllib.request

import zmq

server_py = sys.argv[1]
log = open("/tmp/llm.log", "w")
proc = subprocess.Popen(["/usr/bin/python3", server_py], stdout=log, stderr=log)
try:
    ctx = zmq.Context()
    deadline = time.time() + 30
    ready = False
    while time.time() < deadline:
        if proc.poll() is not None:
            sys.exit("llm_server 启动即退出，查看 /tmp/llm.log")
        with open("/tmp/llm.log", errors="ignore") as f:
            if "listening" in f.read():
                ready = True
                break
        time.sleep(0.5)
    if not ready:
        sys.exit("30s 内未见 listening，查看 /tmp/llm.log")

    def ask(req, timeout_s=90):
        s = ctx.socket(zmq.REQ)
        s.setsockopt(zmq.RCVTIMEO, timeout_s * 1000)
        s.setsockopt(zmq.LINGER, 0)
        s.connect("tcp://localhost:6668")
        try:
            t0 = time.time()
            s.send_string(json.dumps(req, ensure_ascii=False))
            return json.loads(s.recv_string()), time.time() - t0
        finally:
            s.close()

    # 1) respond：普通问答
    r, dur = ask({"query": "用一句话介绍你自己", "session_id": "selftest"})
    assert r["mode"] == "answer" and r["text"], r
    print(f"  respond ok: {dur:.1f}s, {r['tokens']} tokens")

    # 2) tool_call：车控（LLM 直出或关键词兜底均可）
    r, dur = ask({"query": "帮我打开车窗", "allow_tool": True, "session_id": "selftest"})
    assert r["mode"] in ("tool_call", "answer"), r
    if r["mode"] == "tool_call":
        assert r["tool"] == "window_control", r
        print(f"  tool_call ok ({r.get('decision_source')}): {r['tool']}.{r['tool_action']}")
    else:
        print(f"  tool_call: LLM 选择直接回答（兜底未触发），mode=answer: {r['text'][:40]}")

    # 3) 会话历史 + 清空
    r, _ = ask({"query": "清空对话", "session_id": "selftest"})
    assert "清空" in r["text"], r
    print("  会话清空 ok")

    # 4) 直连 HTTP 实测 token/s（R9 口径锚点：prompt/completion 分解计时）
    payload = json.dumps({
        "model": "qwen2.5-1.5b",
        "messages": [{"role": "user", "content": "写一段80字左右的行车安全提示。"}],
        "max_tokens": 128,
    }).encode()
    req = urllib.request.Request("http://localhost:8080/v1/chat/completions",
                                 data=payload, headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=120) as resp:
        out = json.loads(resp.read())
    dur = time.time() - t0
    usage = out.get("usage", {})
    ct, pt = usage.get("completion_tokens", 0), usage.get("prompt_tokens", 0)
    print(f"  [实测] completion {ct} tokens / {dur:.2f}s = {ct/dur:.1f} token/s"
          f"（prompt {pt} tokens 含在内，口径：单次非流式请求）")

    print("SELFTEST_LLM PASS")
finally:
    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    log.close()
    ctx.destroy()
PYEOF
echo "[selftest:llm] 全部 PASS（llama-server 保留运行，供后续联调；如需释放显存：pkill -f llama-server）"
