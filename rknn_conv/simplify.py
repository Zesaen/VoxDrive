"""onnxsim 化简（折叠静态 If 等）+ batch 冻结。
用法: python simplify.py <in.onnx> <out.onnx>
"""
import sys

import numpy as np
import onnx
import onnxruntime as ort
from onnxsim import simplify

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)

# 先冻结 'N' → 1（RKNN 要静态 shape；冻结后再 sim，让 If 条件完全静态）
n = 0
for g in (m.graph,):
    for t in list(g.input) + list(g.output):
        for d in t.type.tensor_type.shape.dim:
            if d.dim_param == "N":
                d.dim_param = ""
                d.dim_value = 1
                n += 1
print(f"frozen {n} dims")

model_sim, check = simplify(m)
assert check, "onnxsim check failed"
onnx.save(model_sim, dst)

ops = [op.op_type for op in model_sim.graph.node]
print("ops:", ops)
assert "If" not in ops and "Loop" not in ops and "Scan" not in ops, "仍有控制流算子"

# 用 ORT 冒烟：确认输出随输入变化（反“常数折叠”）
sess = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
feeds = {}
for i in sess.get_inputs():
    shape = [d if isinstance(d, int) else 1 for d in i.shape]
    if i.type == "tensor(int64)":
        feeds[i.name] = np.random.randint(0, 100, size=shape).astype(np.int64)
    else:
        feeds[i.name] = np.random.randn(*shape).astype(np.float32)
o1 = sess.run(None, feeds)
for k in feeds:
    if feeds[k].dtype == np.int64:
        feeds[k] = feeds[k] + 1
    else:
        feeds[k] = feeds[k] * 1.5 + 0.3
o2 = sess.run(None, feeds)
diff = max(float(np.abs(a - b).max()) for a, b in zip(o1, o2))
print(f"ORT smoke: out varies (maxdiff={diff:.4f}) -> {'OK' if diff > 1e-4 else 'SUSPECT CONSTANT!'}")
print(f"saved {dst}")
