"""决定性实验：同进程内 fp32 编码 vs RK 注入逐块断言相等 + 注入解码"""
import sys

import numpy as np
import onnxruntime as ort

MD = "rknn_conv/models/asr-zipformer-zh-en"
CHUNK, WIN = 32, 39
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31

RK_DUMP = np.load("rknn_conv/rk_enc_outs_fp16.npy")
from rknnlite.api import RKNNLite
rk_enc = RKNNLite()
assert rk_enc.load_rknn("/tmp/encoder_sim_int8.rknn") == 0
rk_enc.init_runtime(core_mask=RKNNLite.NPU_CORE_0)
enc = ort.InferenceSession(f"{MD}/encoder-epoch-99-avg-1.onnx", providers=["CPUExecutionProvider"])
dec = ort.InferenceSession(f"{MD}/decoder-epoch-99-avg-1.onnx", providers=["CPUExecutionProvider"])
joi = ort.InferenceSession(f"{MD}/joiner-epoch-99-avg-1.onnx", providers=["CPUExecutionProvider"])
order = [i.name for i in enc.get_inputs()]

id2tok = {}
for line in open(f"{MD}/tokens.txt", encoding="utf-8"):
    p = line.rstrip("\n").split(" ")
    if len(p) >= 2:
        id2tok[int(p[-1])] = p[0]


def init_states():
    st = {}
    for i, L in enumerate(NUM_LAYERS):
        st[f"cached_len_{i}"] = np.zeros((L, 1), dtype=np.int64)
        st[f"cached_avg_{i}"] = np.zeros((L, 1, ENC), dtype=np.float32)
        st[f"cached_key_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT), dtype=np.float32)
        st[f"cached_val_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
        st[f"cached_val2_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
        st[f"cached_conv1_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
        st[f"cached_conv2_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
    return st


with open("rknn_conv/models/feat_0.bin", "rb") as f:
    T, dim = np.fromfile(f, dtype=np.int32, count=2)
    feat = np.fromfile(f, dtype=np.float32, count=T * dim).reshape(T, dim)

states = init_states()
hyp = []
t = 0
ci = 0
maxdiff_all = 0.0
while t < T:
    win = feat[t:t + WIN]
    if win.shape[0] < WIN:
        win = np.concatenate([win, np.repeat(win[-1:], WIN - win.shape[0], 0)], 0)
    feeds = {"x": win[np.newaxis].astype(np.float32)}
    feeds.update(states)
    outs = enc.run(None, feeds)
    pc_states = dict(zip(order[1:], outs[1:]))
    enc_fp32 = outs[0].reshape(-1, 512)
    # 诊断模式：RK 编码器吃 PC 的正确状态（隔离状态回传损伤）
    rk_feeds = [feeds["x"]] + [pc_states[k].astype(np.float32) for k in order[1:]]
    rk_outs = rk_enc.inference(inputs=rk_feeds)
    rk = np.asarray(rk_outs[0]).reshape(-1, 512).astype(np.float32)
    # RK 返回的新状态回传给下一块（若 RK 状态本身损坏，误差会累积）
    d = float(np.abs(enc_fp32 - rk).max())
    maxdiff_all = max(maxdiff_all, d)
    states = dict(zip(order[1:], [np.asarray(o) for o in rk_outs[1:]]))
    cs = float((enc_fp32*rk).sum()/(np.linalg.norm(enc_fp32)*np.linalg.norm(rk)+1e-9))
    print(f"chunk {ci:2d}: maxdiff={d:.4f} cos={cs:.5f}")
    enc_out = rk  # 用 RK 注入值解码
    for fi in range(enc_out.shape[0]):
        for _ in range(10):
            ctx = np.array([[hyp[-2] if len(hyp) >= 2 else 0,
                             hyp[-1] if len(hyp) >= 1 else 0]], dtype=np.int64)
            dd = dec.run(None, {"y": ctx})[0]
            logits = joi.run(None, {"encoder_out": enc_out[fi][np.newaxis],
                                    "decoder_out": dd.reshape(1, 512)})[0]
            tok = int(np.argmax(logits))
            if tok == 0:
                break
            hyp.append(tok)
    t += CHUNK
    ci += 1
print(f"maxdiff all chunks: {maxdiff_all}")
text = "".join(id2tok.get(x, "") for x in hyp if x != 0).replace("▁", " ").strip()
print("INJECT-DECODE TRANSCRIPT:", text if text else "(空)")
