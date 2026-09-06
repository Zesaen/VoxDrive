#!/usr/bin/env python3
"""wav_push — WAV PCM 注入器（RK NPU ASR 验收工具）

把 16kHz/16bit/mono wav 按 32ms 块实时 pacing 推到 RK voice_service 的 PULL 口，
模拟 mic_stream 采音。用于与 Jetson sherpa fp32 基线做同口径转写对照。
用法: python3 wav_push.py <wav> [push_endpoint]
      push_endpoint 缺省 tcp://<rk.ip>:6711（读 voxdrive.conf）
"""
import sys
import time
import wave

import numpy as np
import zmq

wav_path = sys.argv[1]
ep = sys.argv[2] if len(sys.argv) > 2 else None

if not ep:
    conf = {}
    for cand in ("jetson/config/voxdrive.conf",
                 "/home/nvidia/Desktop/VoxDrive/jetson/config/voxdrive.conf"):
        try:
            for line in open(cand, encoding="utf-8"):
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
            break
        except OSError:
            continue
    ep = f"tcp://{conf.get('rk.ip', '192.168.137.200')}:{conf.get('rk.asr_port', '6711')}"

with wave.open(wav_path, "rb") as w:
    sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
    assert sr == 16000 and ch == 1 and sw == 2, f"需 16k/16bit/mono，实际 {sr}/{ch}ch/{sw*8}bit"
    pcm = np.frombuffer(w.readframes(n), "<i2")

print(f"[wav_push] {wav_path}: {n/sr:.1f}s → {ep}")
ctx = zmq.Context()
push = ctx.socket(zmq.PUSH)
push.setsockopt(zmq.SNDTIMEO, 2000)
push.setsockopt(zmq.LINGER, 0)
push.connect(ep)
time.sleep(0.3)  # 慢加入对端握手

CHUNK = 512
t0 = time.time()
for i in range(0, len(pcm), CHUNK):
    block = pcm[i:i + CHUNK].astype(np.float32) / 32768.0
    try:
        push.send(block.tobytes(), zmq.DONTWAIT)
    except zmq.Again:
        print("[wav_push] 对端积压，等待")
        push.send(block.tobytes())
    # 实时 pacing（多留 0.5s 尾部给终点检测）
    target = (i + CHUNK) / sr + 1.5
    while time.time() - t0 < target:
        time.sleep(0.01)

print(f"[wav_push] 完成，共 {len(pcm)/sr:.1f}s（含 1.5s 尾静音触发终点），观察 voice_service 日志/应答")
ctx.destroy(linger=0)
