"""decoder int32 直入版：simplify 折 If → 输入 y int64→int32（Gather 索引，不量化）。
用法: python decoder_i32.py <decoder_b1.onnx> <decoder_i32.onnx>
"""
import sys

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto
from onnxsim import simplify

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)
m_sim, ok = simplify(m)
assert ok
m = m_sim
ops = [o.op_type for o in m.graph.node]
assert "If" not in ops and "Loop" not in ops, ops

for i in m.graph.input:
    if i.type.tensor_type.elem_type == TensorProto.INT64:
        i.type.tensor_type.elem_type = TensorProto.INT32
        print("input -> int32:", i.name)
    for d in i.type.tensor_type.shape.dim:
        if d.dim_param == "N":
            d.dim_param = ""
            d.dim_value = 1
            print("frozen N -> 1:", i.name)
for o in m.graph.output:
    for d in o.type.tensor_type.shape.dim:
        if d.dim_param == "N":
            d.dim_param = ""
            d.dim_value = 1
onnx.checker.check_model(m)
onnx.save(m, dst)

sess = ort.InferenceSession(dst, providers=["CPUExecutionProvider"])
o1 = sess.run(None, {"y": np.array([[3, 7]], dtype=np.int32)})[0]
o2 = sess.run(None, {"y": np.array([[5, 2]], dtype=np.int32)})[0]
ref = ort.InferenceSession(src.replace("_b1", "-epoch-99-avg-1").replace(
    "decoder_i32", "asr-zipformer-zh-en/decoder-epoch-99-avg-1")
    if False else
    "rknn_conv/models/asr-zipformer-zh-en/decoder-epoch-99-avg-1.onnx",
    providers=["CPUExecutionProvider"]).run(
        None, {"y": np.array([[3, 7]], dtype=np.int64)})[0]
print("vs int64-orig maxdiff:", float(np.abs(o1 - ref).max()))
print(f"smoke vary: {not np.allclose(o1, o2)}")
print("saved", dst)
