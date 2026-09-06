#!/usr/bin/env python3
"""vlm_service — R15 画面问答（Qwen2-VL-2B，llama-server mtmd，Jetson）

语音"画面里有什么"→ router 直通 REP :vlm.port → 本服务：
  1. 抓行车记录仪最新帧（RK :6700 snapshot envelope，JPEG b64）
  2. 宿主侧降采样（默认 960×540：1080p 原图 ~2700 image token 会爆 2k 上下文，
     降 4 倍后 ~700 token，实测零 cudaMalloc 失败）
  3. 确保 VLM llama-server 在跑：不在则停 llm 的 llama-server → evict 其模型页缓存
     （posix_fadvise，无特权替代 drop_caches——Jetson 统一内存下页缓存会挤爆
     cudaMalloc，实测不清必 OOM）→ 起 VLM（-ngl 99）→ 等 /health
  4. POST /v1/chat/completions（OpenAI 图像格式）取应答
  5. 恢复：停 VLM → evict VLM 模型页 → 按 start_core 同款参数重启 llm llama-server

GPU 内存实测定式（2026-09-06）：全栈共存不可行（mmproj 677MB 即 OOM）；
换出后也必须 evict 页缓存；上下文 2048+降采样图是能装下的组合（8192 会崩）。
"""
import base64
import io
import json
import os
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

import zmq

stop_flag = False

MODELS_DIR = os.path.expanduser("~/Desktop/VoxDrive/models")
LOG_PREFIX = "[vlm] "


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {LOG_PREFIX}{msg}", flush=True)


def _load_conf():
    kv = {}
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.environ.get("VOX_CONF"),
                 os.path.join(here, "..", "..", "config", "voxdrive.conf"),
                 os.path.expanduser("~/Desktop/VoxDrive/jetson/config/voxdrive.conf")):
        if cand and os.path.isfile(cand):
            for line in open(cand, encoding="utf-8"):
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
            return kv
    return kv


CONF = _load_conf()


def conf(key, default):
    return CONF.get(key, default)


def expand(p):
    """conf 值里的 $HOME 展开（expanduser 只认 ~，不认 $HOME）"""
    if not p:
        return p
    if p.startswith("$HOME"):
        p = os.environ.get("HOME", "") + p[5:]
    return os.path.expanduser(p)


def evict_pages(paths):
    """posix_fadvise(DONTNEED) 清文件页缓存（须在占用进程退出后调用；无特权）"""
    for p in paths:
        try:
            fd = os.open(p, os.O_RDONLY)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            os.close(fd)
        except OSError:
            pass


def http_ok(port, timeout=3):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=timeout) as r:
            return r.status == 200
    except Exception:
        return False


def kill_llama_servers():
    subprocess.run(["pkill", "-9", "-x", "llama-server"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)


def wait_health(port, timeout_s, tag):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if http_ok(port):
            log(f"{tag} 就绪 ({time.time()-t0:.1f}s)")
            return True
        time.sleep(1.5)
    log(f"{tag} 启动超时（{timeout_s}s）")
    return False


def snapshot_jpeg_b64():
    """RK 行车记录仪最新帧（snapshot envelope → payload.jpeg_b64）"""
    rk_ip = conf("rk.ip", "192.168.137.200")
    rk_port = conf("rk.status_port", "6700")
    ctx = zmq.Context()
    s = ctx.socket(zmq.REQ)
    s.setsockopt(zmq.RCVTIMEO, 5000)
    s.setsockopt(zmq.LINGER, 0)
    s.connect(f"tcp://{rk_ip}:{rk_port}")
    s.send_string(json.dumps({"version": 1, "type": "snapshot",
                              "timestamp_ms": int(time.time() * 1000),
                              "source": "jetson.vlm", "payload": {"cmd": "snapshot"}}))
    rep = json.loads(s.recv_string())
    s.close()
    ctx.destroy(linger=0)
    b64 = rep.get("payload", {}).get("jpeg_b64", "")
    if not b64:
        raise RuntimeError("snapshot 无图像数据")
    return b64


def downscale_b64(jpeg_b64):
    from PIL import Image
    img = Image.open(io.BytesIO(base64.b64decode(jpeg_b64))).convert("RGB")
    img = img.resize((int(conf("vlm.img_w", "960")), int(conf("vlm.img_h", "540"))))
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=85)
    return base64.b64encode(buf.getvalue()).decode()


def ensure_vlm_server():
    """VLM llama-server 按需拉起（含 llm 换出与页缓存 evict）；返回是否就绪"""
    vport = conf("vlm.http_port", "8801")
    if http_ok(vport):
        return True
    kill_llama_servers()  # 换出 llm（此刻 VLM 必不在跑，-x 通杀无误伤）
    evict_pages([expand(conf("llm.model", "")),
                 *[os.path.join(MODELS_DIR, f) for f in os.listdir(MODELS_DIR)
                   if f.endswith((".gguf", ".bin"))]])
    bin_ = expand(conf("llm.server_bin",
                       "$HOME/Desktop/VoxDrive/third_party/llama.cpp/build/bin/llama-server"))
    cmd = ["env", "GGML_CUDA_NO_VMM=1", bin_,
           "-m", expand(conf("vlm.model", f"{MODELS_DIR}/qwen2vl-2b-q4km.gguf")),
           "--mmproj", expand(conf("vlm.mmproj", f"{MODELS_DIR}/qwen2vl-2b-mmproj-q8_0.gguf")),
           "-ngl", "99", "-c", conf("vlm.ctx", "2048"),
           "--host", "127.0.0.1", "--port", vport]
    log(f"起 VLM llama-server (ctx={conf('vlm.ctx', '2048')})")
    with open("/tmp/vlm_server.log", "ab") as lf:
        subprocess.Popen(cmd, stdout=lf, stderr=lf, start_new_session=True)
    return wait_health(vport, 150, "VLM")


def ask_vlm(question, jpeg_b64):
    body = json.dumps({
        "messages": [{"role": "user", "content": [
            {"type": "text",
             "text": "你是车载语音助手。用户在问行车记录仪当前画面，请依据图片用一两句自然中文回答，不要描述与画面无关的内容。"},
            {"type": "image_url",
             "image_url": {"url": "data:image/jpeg;base64," + jpeg_b64}}]}],
        "max_tokens": 150, "temperature": 0.1}).encode()
    req = urllib.request.Request(
        f"http://127.0.0.1:{conf('vlm.http_port', '8801')}/v1/chat/completions",
        data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=240) as r:
        out = json.loads(r.read())
    return out["choices"][0]["message"]["content"].strip()


def restore_llm_server():
    """按 start_core 同款参数恢复 llm llama-server（失败记日志，不阻塞应答）"""
    kill_llama_servers()  # 停 VLM
    evict_pages([expand(conf("vlm.model", "")), expand(conf("vlm.mmproj", "")),
                 expand(conf("llm.model", ""))])
    bin_ = expand(conf("llm.server_bin",
                       "$HOME/Desktop/VoxDrive/third_party/llama.cpp/build/bin/llama-server"))
    cmd = ["env", "GGML_CUDA_NO_VMM=1", bin_,
           "-m", expand(conf("llm.model", "")),
           "-ngl", conf("llm.ngl", "28"), "-c", conf("llm.ctx", "2048"),
           "--host", "127.0.0.1", "--port", conf("port.llama_http", "8080")]
    log("恢复 llm llama-server")
    with open("/tmp/llama-server.log", "ab") as lf:
        subprocess.Popen(cmd, stdout=lf, stderr=lf, start_new_session=True)
    if not wait_health(conf("port.llama_http", "8080"), 120, "llm"):
        log("ERROR: llm llama-server 恢复失败，LLM 查询将不可用（可跑 start_core.sh 重启全栈）")


def handle(question):
    t0 = time.time()
    jb64 = downscale_b64(snapshot_jpeg_b64())
    t_snap = time.time() - t0
    if not ensure_vlm_server():
        return {"ok": False, "err": "VLM 启动失败（GPU 内存不足？）"}
    t_load = time.time()
    try:
        ans = ask_vlm(question, jb64)
    except Exception as e:
        restore_llm_server()
        return {"ok": False, "err": f"VLM 推理失败: {e}"}
    t_inf = time.time() - t_load
    restore_llm_server()
    log(f"快照 {t_snap:.2f}s + 推理 {t_inf:.1f}s + 恢复 {time.time()-t0-t_snap-t_inf:.1f}s → {ans[:60]}")
    return {"ok": True, "text": ans,
            "ms": int((time.time() - t0) * 1000)}


def main():
    signal.signal(signal.SIGINT, lambda *_: globals().__setitem__("stop_flag", True))
    port = conf("vlm.port", "6721")
    ctx = zmq.Context()
    rep = ctx.socket(zmq.REP)
    rep.setsockopt(zmq.RCVTIMEO, 200)
    rep.setsockopt(zmq.LINGER, 0)
    rep.bind(f"tcp://*:{port}")
    log(f"REP :{port} 就绪（VLM 按需拉起：Qwen2-VL-2B + mtmd）")
    while not stop_flag:
        try:
            req = json.loads(rep.recv_string())
        except (zmq.Again, json.JSONDecodeError, ValueError):
            continue
        q = str(req.get("text", "")).strip()
        if not q:
            rep.send_string(json.dumps({"ok": False, "err": "empty text"}))
            continue
        log(f"query: {q!r}")
        rep.send_string(json.dumps(handle(q), ensure_ascii=False))
    log("退出")


if __name__ == "__main__":
    main()
