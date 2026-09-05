"""PC fp32 全流程参考（可注入 RK encoder 输出）。
用法: python pc_loop_test.py            # 全 fp32 参考，保存 pc_chunks.npy
      python pc_loop_test.py rkenc      # 注入 rk_enc_outs_fp16.npy / rk_enc_outs.npy
"""
import sys

import numpy as np
import onnxruntime as ort

MD = "rknn_conv/models/asr-zipformer-zh-en"
CHUNK, WIN = 32, 39
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31

USE_RK = len(sys.argv) > 1 and sys.argv[1] == "rkenc"
RK_DUMP = None
if USE_RK:
    for cand in ("rknn_conv/rk_enc_outs_fp16.npy", "rknn_conv/rk_enc_outs.npy"):
        try:
            RK_DUMP = np.load(cand)
            print("injecting", cand, RK_DUMP.shape)
            break
        except FileNotFoundError:
            pass
    assert RK_DUMP is not None

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
all_o = []
t = 0
while t < T:
    if RK_DUMP is not None:
        k = len(all_o) * 8
        if k >= len(RK_DUMP):
            break
        enc_out = RK_DUMP[k:k + 8]
    else:
        win = feat[t:t + WIN]
        if win.shape[0] < WIN:
            win = np.concatenate([win, np.repeat(win[-1:], WIN - win.shape[0], 0)], 0)
        feeds = {"x": win[np.newaxis].astype(np.float32)}
        feeds.update(states)
        outs = enc.run(None, feeds)
        states = dict(zip(order[1:], outs[1:]))
        enc_out = outs[0].reshape(-1, 512)
    all_o.append(enc_out)
    for fi in range(enc_out.shape[0]):
        for _ in range(10):
            ctx = np.array([[hyp[-2] if len(hyp) >= 2 else 0,
                             hyp[-1] if len(hyp) >= 1 else 0]], dtype=np.int64)
            d = dec.run(None, {"y": ctx})[0]
            logits = joi.run(None, {"encoder_out": enc_out[fi][np.newaxis],
                                    "decoder_out": d.reshape(1, 512)})[0]
            tok = int(np.argmax(logits))
            if True:
                print(f"DBG c{len(all_o)} f{fi} tok={tok} logit_top3={np.argsort(logits.ravel())[-3:][::-1]} vals={np.sort(logits.ravel())[-3:]}")
            if tok == 0:
                break
            hyp.append(tok)
    t += CHUNK

print("hyp:", hyp[:24])
np.save("rknn_conv/pc_chunks.npy", np.concatenate(all_o, 0))
text = "".join(id2tok.get(x, "") for x in hyp if x != 0).replace("▁", " ").strip()
print("TRANSCRIPT:", text if text else "(空)")
