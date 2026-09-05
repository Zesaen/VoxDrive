"""decoder 最终转换前处理：
  1) simplify(原 b1)——int64 输入下静态 If 被折叠（已验证）
  2) 输入 y: int64 → float32 'y_f'；Gather(embedding) → Equal/Cast/MatMul 纯 float one-hot
  3) 再 simplify + ORT 冒烟（喂整数 float 保证 one-hot 命中）
用法: python decoder_final.py <decoder_b1.onnx> <decoder_final.onnx>
"""
import sys

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper
from onnxsim import simplify

src, dst = sys.argv[1], sys.argv[2]

m = onnx.load(src)
m_sim, ok = simplify(m)
assert ok
m = m_sim  # 后续全部在化简后的图上操作
ops = [o.op_type for o in m_sim.graph.node]
assert "If" not in ops and "Loop" not in ops, f"control flow left: {ops}"
print("step1 sim ops:", ops)

g = m_sim.graph
# 输入 y int64 → y_f float32
old_in = next(i for i in g.input if i.type.tensor_type.elem_type == TensorProto.INT64)
y_old = old_in.name
dims = [(d.dim_value if d.dim_value else 1) for d in old_in.type.tensor_type.shape.dim]
del g.input[:]
g.input.extend([helper.make_tensor_value_info("y_f", TensorProto.FLOAT, dims)])

# 找 Gather（其索引即 y），取 embedding 权重
gather = next(n for n in g.node if n.op_type == "Gather")
W = numpy_helper.to_array(next(t for t in g.initializer if t.name == gather.input[0]))
V, D = W.shape
g_out = gather.output[0]
g.node.remove(gather)

new_nodes = [
    helper.make_node("Reshape", ["y_f", "oh_shape"], ["y_r"], name="oh_reshape"),
    helper.make_node("Equal", ["y_r", "oh_range"], ["oh_eq"], name="oh_eq"),
    helper.make_node("Cast", ["oh_eq"], ["oh_f"], to=TensorProto.FLOAT, name="oh_cast"),
    helper.make_node("MatMul", ["oh_f", gather.input[0]], [g_out], name="oh_mm"),
]
idx = list(g.node).index(next(n for n in g.node if y_old in n.input)) if any(
    y_old in n.input for n in g.node) else 0
for k, n in enumerate(new_nodes):
    g.node.insert(idx + k, n)
g.initializer.append(numpy_helper.from_array(
    np.arange(V, dtype=np.float32).reshape(1, 1, V), "oh_range"))
g.initializer.append(numpy_helper.from_array(
    np.array([dims[0], dims[1], 1], dtype=np.int64), "oh_shape"))
# 残留对 y_old 的引用（若有）接到 y_f
for n in g.node:
    for k, inp in enumerate(n.input):
        if inp == y_old:
            n.input[k] = "y_f"

onnx.checker.check_model(m)
onnx.save(m, dst)
ops2 = [o.op_type for o in m.graph.node]
print("step2 ops:", ops2)
assert not any(o in ops2 for o in ("If", "Loop", "Gather")), "仍有不支持模式"

# ORT 冒烟：整数 float 输入保证 one-hot 精确命中
sess = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
f1 = {i.name: np.array([[3.0, 7.0]], dtype=np.float32) for i in sess.get_inputs()}
f2 = {i.name: np.array([[5.0, 2.0]], dtype=np.float32) for i in sess.get_inputs()}
o1 = sess.run(None, f1)[0]
o2 = sess.run(None, f2)[0]
print(f"smoke: shape={o1.shape} vary={not np.allclose(o1, o2)}")
assert not np.allclose(o1, o2)
print(f"saved {dst}")
