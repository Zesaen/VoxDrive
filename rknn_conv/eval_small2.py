"""RK 板 decoder 评测 v2：按模型 quant scale 量化输入、反量化输出。
同时打印 lite2 张量属性接口，防御性适配。
用法: python3 eval_small2.py <dir>
"""
import sys
import time

import numpy as np
from rknnlite.api import RKNNLite

MD = sys.argv[1]
dec = RKNNLite()
assert dec.load_rknn(f"{MD}/decoder_hybrid.rknn") == 0
assert dec.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0

attrs = None
for name in ("get_tensor_attrs", "get_input_attrs", "get_output_attrs"):
    if hasattr(dec, name):
        print("has API:", name)
        attrs = name
if attrs == "get_tensor_attrs":
    ia = dec.get_tensor_attrs(0)
    print("in attrs:", ia)
    scale_in = float(ia[4]) if isinstance(ia, tuple) and len(ia) > 4 else float(getattr(ia, "scale", 1.0))
else:
    scale_in = 1.0

y_f = np.array([[3.0, 7.0]], dtype=np.float32)
ref = np.load(f"{MD}/dec_ref_3_7.npy")


def run(yf):
    yq = np.clip(np.round(yf / scale_in), -128, 127).astype(np.int8)
    o = dec.inference(inputs=[yq])[0]
    o = np.asarray(o)
    if o.dtype == np.int8:
        oa = dec.get_tensor_attrs(0)  # 输出属性按输出索引取（若不支持则退化为按输入）
        try:
            scale_out = float(oa[4]) if isinstance(oa, tuple) and len(oa) > 4 else 1.0
        except Exception:
            scale_out = 1.0
        return o.astype(np.float32) * scale_out
    return o.astype(np.float32)


out = run(y_f).reshape(1, 512)
cos = float(np.dot(out.ravel(), ref.ravel()) /
            (np.linalg.norm(out) * np.linalg.norm(ref) + 1e-9))
print(f"cos_vs_fp32={cos:.4f}  out[:6]={out.ravel()[:6]}  ref[:6]={ref.ravel()[:6]}")
t0 = time.perf_counter()
for _ in range(200):
    run(y_f)
print(f"latency={(time.perf_counter()-t0)/200*1000:.3f}ms")
