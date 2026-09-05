"""RK 板导出全块 encoder 输出（供 PC 用 fp32 decoder/joiner 对照）"""
import sys
import time

import numpy as np
from rknnlite.api import RKNNLite

MD = "/home/cat/rk_asr"
CHUNK, WIN = 32, 39
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31

enc = RKNNLite()
assert enc.load_rknn(f"{MD}/encoder_sim_int8.rknn") == 0
enc.init_runtime(core_mask=RKNNLite.NPU_CORE_0)
order = [l.strip() for l in open(f"{MD}/encoder_input_order.txt") if l.strip()]


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


with open(f"{MD}/feat_0.bin", "rb") as f:
    T, dim = np.fromfile(f, dtype=np.int32, count=2)
    feat = np.fromfile(f, dtype=np.float32, count=T * dim).reshape(T, dim)

states = init_states()
all_out = []
t = 0
t0 = time.perf_counter()
while t < T:
    win = feat[t:t + WIN]
    if win.shape[0] < WIN:
        win = np.concatenate([win, np.repeat(win[-1:], WIN - win.shape[0], 0)], 0)
    feeds = [win[np.newaxis].astype(np.float32)] + [states[k] for k in order[1:]]
    outs = enc.inference(inputs=feeds)
    states = dict(zip(order[1:], [np.asarray(o) for o in outs[1:]]))
    all_out.append(np.asarray(outs[0]).reshape(-1, 512).astype(np.float32))
    t += CHUNK
print(f"chunks {len(all_out)} wall {time.perf_counter()-t0:.2f}s")
np.save(f"{MD}/rk_enc_outs.npy", np.concatenate(all_out, 0))
print("saved rk_enc_outs.npy", np.concatenate(all_out, 0).shape)
