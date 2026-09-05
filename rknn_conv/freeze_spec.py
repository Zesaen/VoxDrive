"""按 sherpa C++ 规格冻结 encoder 全部符号维度（onnxsim 自行推断会冻错 val2/conv 维度），
再化简，并逐项校验输入形状。
用法: python freeze_spec.py <encoder_b1.onnx> <encoder_sim2.onnx>
"""
import sys

import numpy as np
import onnx
import onnxruntime as ort
from onnxsim import simplify

src, dst = sys.argv[1], sys.argv[2]
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31

spec = {"x": [1, 39, 80]}
for i, L in enumerate(NUM_LAYERS):
    spec[f"cached_len_{i}"] = [L, 1]
    spec[f"cached_avg_{i}"] = [L, 1, ENC]
    spec[f"cached_key_{i}"] = [L, LEFT_CTX[i], 1, ATT]
    spec[f"cached_val_{i}"] = [L, LEFT_CTX[i], 1, ATT // 2]
    spec[f"cached_val2_{i}"] = [L, LEFT_CTX[i], 1, ATT // 2]
    spec[f"cached_conv1_{i}"] = [L, 1, ENC, CNN_K - 1]
    spec[f"cached_conv2_{i}"] = [L, 1, ENC, CNN_K - 1]

m = onnx.load(src)
g = m.graph
frozen = 0
for i in g.input:
    name = i.name
    dims = i.type.tensor_type.shape.dim
    want = spec[name]
    assert len(dims) == len(want), f"{name}: rank {len(dims)} != {len(want)}"
    for k, d in enumerate(dims):
        if d.dim_param:  # 符号维度 → 冻结为规格值
            d.dim_param = ""
            d.dim_value = want[k]
            frozen += 1
        else:
            assert d.dim_value == want[k], f"{name}[{k}]: {d.dim_value} != {want[k]}"
print(f"frozen {frozen} symbolic dims")

m_sim, ok = simplify(m)
assert ok
m = m_sim
onnx.save(m, dst)

# ORT 实跑校验全部输入形状（真实前向一遍）
sess = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
feeds = {}
rng = np.random.default_rng(0)
for i in sess.get_inputs():
    if i.type == "tensor(int64)":
        feeds[i.name] = np.zeros(i.shape, dtype=np.int64)
    else:
        feeds[i.name] = rng.standard_normal(i.shape).astype(np.float32) * 0.1
outs = sess.run(None, feeds)
print("ORT forward OK, outputs:", len(outs), "encoder_out:", outs[0].shape)
print("saved", dst)
