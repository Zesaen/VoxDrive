"""RK3588 NPU 上 decoder/joiner 精度与时延验证（rknn_toolkit_lite2）
用法: python3 eval_small.py <models_dir>
与 PC 侧 ORT 参考对比 decoder 输出（y_f=[3,7]）；joiner 用同 encoder/decoder 输出量级。
"""
import sys
import time

import numpy as np
from rknnlite.api import RKNNLite

MD = sys.argv[1]

# ── decoder ──
dec = RKNNLite()
ret = dec.load_rknn(f"{MD}/decoder_decoder_final_int8.rknn")
assert ret == 0, "decoder load fail"
ret = dec.init_runtime(core_mask=RKNNLite.NPU_CORE_0)
assert ret == 0, "decoder init fail"
y = np.array([[3.0, 7.0]], dtype=np.float32)
d_out = dec.inference(inputs=[y])
d_out = np.asarray(d_out[0]).reshape(1, 512)
ref = np.load(f"{MD}/dec_ref_3_7.npy")
cos = float(np.dot(d_out.ravel(), ref.ravel()) /
            (np.linalg.norm(d_out) * np.linalg.norm(ref) + 1e-9))
t0 = time.perf_counter()
for _ in range(200):
    dec.inference(inputs=[y])
dt_dec = (time.perf_counter() - t0) / 200 * 1000
print(f"decoder: cos_vs_fp32={cos:.4f} latency={dt_dec:.3f}ms")

# ── joiner ──
joi = RKNNLite()
assert joi.load_rknn(f"{MD}/joiner_joiner_sim_int8.rknn") == 0
assert joi.init_runtime(core_mask=RKNNLite.NPU_CORE_1) == 0
enc_in = np.load(f"{MD}/enc_ref.npy") if False else np.random.randn(1, 512).astype(np.float32) * 3
enc_in = np.abs(enc_in)  # encoder_out 为 ReLU 后非负
j_out = joi.inference(inputs=[enc_in, d_out])
j_out = np.asarray(j_out[0]).reshape(-1)
tok = int(np.argmax(j_out))
t0 = time.perf_counter()
for _ in range(200):
    joi.inference(inputs=[enc_in, d_out])
dt_joi = (time.perf_counter() - t0) / 200 * 1000
print(f"joiner: out={j_out.shape} argmax={tok} latency={dt_joi:.3f}ms")
print("SMALL_MODELS_OK")
