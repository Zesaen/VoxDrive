"""RK encoder INT8 输出精度：校准样本前向 vs PC fp32 参考"""
import sys
import time

import numpy as np
from rknnlite.api import RKNNLite

MD = "/home/cat/rk_asr"
S = sys.argv[1] if len(sys.argv) > 1 else "enc_00_000"

enc = RKNNLite()
assert enc.load_rknn(f"{MD}/encoder_sim_int8.rknn") == 0
enc.init_runtime(core_mask=RKNNLite.NPU_CORE_0)

order = [l.strip() for l in open(f"{MD}/encoder_input_order.txt") if l.strip()]
d = np.load(f"{MD}/calib_npy/{S}/{order[0]}.npy") if False else None
feeds = []
for k in order:
    feeds.append(np.load(f"{MD}/calib_npy/{S}/{k}.npy"))

t0 = time.perf_counter()
outs = enc.inference(inputs=feeds)
dt = (time.perf_counter() - t0) * 1000
out = np.asarray(outs[0]).reshape(1, 8, 512).astype(np.float32)

ref = np.load(f"{MD}/ref_outs.npz")["0_0"]
cos = float((out * ref).sum() / (np.linalg.norm(out) * np.linalg.norm(ref) + 1e-9))
print(f"encoder INT8 vs fp32: cos={cos:.4f} latency={dt:.1f}ms out[:4]={out.ravel()[:4]} ref[:4]={ref.ravel()[:4]}")
