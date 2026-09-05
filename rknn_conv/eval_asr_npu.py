"""RK3588 NPU 流式 ASR 评测（含 4D 输入布局修复）—— R14 定稿脚本

用法: python3 eval_asr_npu.py <encoder.rknn> <i64|i32> [feat.bin] [--dec8] [--joi8]
      decoder/joiner 默认 fp16（精度定稿），--dec8/--joi8 可换 int8 做精度对照

**关键坑（2026-09-05 实测定位，务必保留）**：
RKNN runtime 对 4D 图输入按与 ONNX 逻辑序不同的内存序消费——
所有 ndim==4 的输入（cached_key/val/val2/conv1/conv2）必须喂
    np.ascontiguousarray(np.transpose(x, (0,2,3,1)))
输出侧恒等（直接按逻辑形状 reshape 即可）。≤3D 输入原样喂。
不修此点症状 = chunk0 正确、非零缓存块起输出结构性错乱（cos≈0.3），
极易误判为「模型转换/量化问题」——实际模型编译完全正确。
另：RKNN 转换后图输入顺序被重排（x + int 输入在前 + float 按名字分组），
见 encoder_input_order.txt，喂食必须按该顺序。

实测（RK3588 NPU 三核分载，feat_0 10.1s 中英混）：
  fp16 三模型  转写与 Jetson ORT fp32 贪心逐字一致，RTF 0.283（纯 enc 0.236）
  int8 encoder 单块 cos 0.92 且 cached_avg 状态 cos 0.594 → 流式累积崩溃
  int8 decoder cos 0.638 / int8 joiner argmax 截断 → 部署选 fp16
"""
import sys
import time

import numpy as np
from rknnlite.api import RKNNLite

MD = "/home/cat/rk_asr"
CHUNK, WIN = 32, 39
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31
T4 = (0, 2, 3, 1)


def main():
    enc_rknn = sys.argv[1]
    len_dt = {"i64": np.int64, "i32": np.int32}[sys.argv[2]]
    feat_path = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("--") else f"{MD}/feat_0.bin"
    dec_rknn = f"{MD}/decoder_decoder_i32_int8.rknn" if "--dec8" in sys.argv else f"{MD}/decoder_fp16.rknn"
    joi_rknn = f"{MD}/joiner_joiner_sim_int8.rknn" if "--joi8" in sys.argv else f"{MD}/joiner_fp16.rknn"

    id2tok = {}
    for line in open(f"{MD}/tokens.txt", encoding="utf-8"):
        p = line.rstrip("\n").split(" ")
        if len(p) >= 2:
            id2tok[int(p[-1])] = p[0]

    enc = RKNNLite()
    assert enc.load_rknn(enc_rknn) == 0
    assert enc.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0
    dec = RKNNLite()
    assert dec.load_rknn(dec_rknn) == 0
    assert dec.init_runtime(core_mask=RKNNLite.NPU_CORE_1) == 0
    joi = RKNNLite()
    assert joi.load_rknn(joi_rknn) == 0
    assert joi.init_runtime(core_mask=RKNNLite.NPU_CORE_2) == 0

    order = [l.strip() for l in open(f"{MD}/encoder_input_order.txt") if l.strip()]
    assert len(order) == 36

    def init_states():
        st = {}
        for i, L in enumerate(NUM_LAYERS):
            st[f"cached_len_{i}"] = np.zeros((L, 1), dtype=len_dt)
            st[f"cached_avg_{i}"] = np.zeros((L, 1, ENC), dtype=np.float32)
            st[f"cached_key_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT), dtype=np.float32)
            st[f"cached_val_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
            st[f"cached_val2_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
            st[f"cached_conv1_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
            st[f"cached_conv2_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
        return st

    def decode_frame(frame, hyp):
        for _ in range(10):
            ctx = np.array([[hyp[-2] if len(hyp) >= 2 else 0,
                             hyp[-1] if len(hyp) >= 1 else 0]], dtype=np.int32)
            d = np.asarray(dec.inference(inputs=[ctx])[0]).reshape(1, 512).astype(np.float32)
            logits = np.asarray(joi.inference(inputs=[frame[np.newaxis], d])[0]).reshape(-1)
            tok = int(np.argmax(logits))
            if tok == 0:
                return
            hyp.append(tok)

    with open(feat_path, "rb") as f:
        T, dim = np.fromfile(f, dtype=np.int32, count=2)
        feat = np.fromfile(f, dtype=np.float32, count=T * dim).reshape(T, dim)
    print(f"feat: {T} frames ({T * 0.01:.1f}s) enc={enc_rknn.split('/')[-1]} len={len_dt.__name__}")

    states = init_states()
    hyp = []
    t = 0
    t_enc = 0.0
    t0 = time.perf_counter()
    while t < T:
        win = feat[t:t + WIN]
        if win.shape[0] < WIN:
            win = np.concatenate([win, np.repeat(win[-1:], WIN - win.shape[0], 0)], 0)
        feeds = [win[np.newaxis].astype(np.float32)]
        for k in order[1:]:
            a = states[k]
            feeds.append(np.ascontiguousarray(np.transpose(a, T4)) if a.ndim == 4 else a)
        tc = time.perf_counter()
        outs = enc.inference(inputs=feeds)
        t_enc += time.perf_counter() - tc
        enc_out = np.asarray(outs[0]).reshape(-1, 512).astype(np.float32)
        states = dict(zip(order[1:], [np.asarray(o) for o in outs[1:]]))
        for fi in range(enc_out.shape[0]):
            decode_frame(enc_out[fi], hyp)
        t += CHUNK
    el = time.perf_counter() - t0
    text = "".join(id2tok.get(x, "") for x in hyp if x != 0).replace("▁", " ").strip()
    print(f"TRANSCRIPT: {text}")
    print(f"total={el:.2f}s RTF={el / (T * 0.01):.3f} (纯enc RTF={t_enc / (T * 0.01):.3f})")


if __name__ == "__main__":
    main()
