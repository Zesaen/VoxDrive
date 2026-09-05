"""RK3588 NPU 全链路流式 ASR 贪心解码评测（encoder/decoder/joiner 三模型分核）
用法: python3 eval_asr_greedy.py <dir> [feat_bin]
依赖: <dir>/encoder_sim_int8.rknn decoder_fp16.rknn joiner_fp16.rknn
      <dir>/encoder_input_order.txt tokens.txt feat_0.bin
"""
import sys
import time

import numpy as np
from rknnlite.api import RKNNLite

MD = sys.argv[1] if len(sys.argv) > 1 else "/home/cat/rk_asr"
FEAT = sys.argv[2] if len(sys.argv) > 2 else f"{MD}/feat_0.bin"

CHUNK, LOOKAHEAD = 32, 7
WIN = CHUNK + LOOKAHEAD
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31

# ── tokens ──
id2tok = {}
for line in open(f"{MD}/tokens.txt", encoding="utf-8"):
    parts = line.rstrip("\n").split(" ")
    if len(parts) >= 2:
        id2tok[int(parts[-1])] = parts[0]
BLANK = 0

# ── 模型 ──
enc = RKNNLite()
assert enc.load_rknn(f"{MD}/encoder_sim_int8.rknn") == 0
assert enc.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0
dec = RKNNLite()
assert dec.load_rknn(f"{MD}/decoder_fp16.rknn") == 0
assert dec.init_runtime(core_mask=RKNNLite.NPU_CORE_1) == 0
joi = RKNNLite()
assert joi.load_rknn(f"{MD}/joiner_fp16.rknn") == 0
assert joi.init_runtime(core_mask=RKNNLite.NPU_CORE_2) == 0

order = [l.strip() for l in open(f"{MD}/encoder_input_order.txt") if l.strip()]
assert len(order) == 36, len(order)


def init_states():
    st = {}
    for i, L in enumerate(NUM_LAYERS):
        st[f"cached_len_{i}"] = np.zeros((L, 1), dtype=np.int32)
        st[f"cached_avg_{i}"] = np.zeros((L, 1, ENC), dtype=np.float32)
        st[f"cached_key_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT), dtype=np.float32)
        st[f"cached_val_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
        st[f"cached_val2_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
        st[f"cached_conv1_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
        st[f"cached_conv2_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
    return st


_dbg = [0]


def decode_frame(enc_frame, hyp):
    """单帧贪心 transducer 解码（decoder/joiner NPU）"""
    for _ in range(10):  # 单帧最大 emit
        ctx = np.array([[hyp[-2] if len(hyp) >= 2 else 0,
                         hyp[-1] if len(hyp) >= 1 else 0]], dtype=np.int32)
        d = np.asarray(dec.inference(inputs=[ctx])[0]).reshape(1, 512).astype(np.float32)
        logits = np.asarray(joi.inference(inputs=[enc_frame[np.newaxis], d])[0]).reshape(-1)
        tok = int(np.argmax(logits))
        _dbg[0] += 1
        if _dbg[0] <= 300:
            print(f"DBG n{_dbg[0]} tok={tok} top3={np.argsort(logits)[-3:][::-1]} vals={np.sort(logits)[-3:]}")
        if tok == BLANK:
            return
        hyp.append(tok)


def main():
    with open(FEAT, "rb") as f:
        T, dim = np.fromfile(f, dtype=np.int32, count=2)
        feat = np.fromfile(f, dtype=np.float32, count=T * dim).reshape(T, dim)
    dur_s = T * 0.01
    print(f"feat: {T} frames ({dur_s:.1f}s audio)")

    states = init_states()
    hyp = []
    t = 0
    n_chunks = 0
    t_enc = 0.0
    t0 = time.perf_counter()
    while t < T:
        win = feat[t:t + WIN]
        if win.shape[0] < WIN:
            win = np.concatenate([win, np.repeat(win[-1:], WIN - win.shape[0], 0)], 0)
        feeds = [win[np.newaxis].astype(np.float32)] + [states[k] for k in order[1:]]
        tc = time.perf_counter()
        outs = enc.inference(inputs=feeds)
        t_enc += time.perf_counter() - tc
        enc_out = np.asarray(outs[0]).reshape(-1, 512).astype(np.float32)
        new_states = dict(zip(order[1:], [np.asarray(o) for o in outs[1:]]))
        states = new_states
        for fi in range(enc_out.shape[0]):
            decode_frame(enc_out[fi], hyp)
        t += CHUNK
        n_chunks += 1
    wall = time.perf_counter() - t0

    text = "".join(id2tok.get(t2, "") for t2 in hyp if t2 != BLANK)
    text = text.replace("▁", " ").strip()
    print(f"chunks={n_chunks} tokens={len(hyp)}")
    print(f"encoder: {t_enc/n_chunks*1000:.1f}ms/chunk | 总耗时 {wall:.2f}s | RTF={wall/dur_s:.3f}")
    print("TRANSCRIPT:", text if text else "(空)")


main()
