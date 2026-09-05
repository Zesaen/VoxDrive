"""把 ONNX 图的 int64 输入改为 float32 输入 + 图首 Cast(to=INT64)。
RKNN 不支持 int64 图输入当真数据（会被当常数折叠→输出恒定）。
用法: python fix_int64_inputs.py <in.onnx> <out.onnx>
"""
import sys

import onnx
from onnx import TensorProto, helper

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)
g = m.graph

new_inputs, casts = [], []
for i in g.input:
    tt = i.type.tensor_type
    if tt.elem_type == TensorProto.INT64:
        dims = [(d.dim_value if d.dim_value else 1) for d in tt.shape.dim]
        vi = helper.make_tensor_value_info(i.name + "_f", TensorProto.FLOAT, dims)
        cast = helper.make_node("Cast", [i.name + "_f"], [i.name],
                                to=TensorProto.INT32, name=f"fixcast_{i.name}")
        new_inputs.append(vi)
        casts.append(cast)
    else:
        new_inputs.append(i)

del g.input[:]
g.input.extend(new_inputs)
for c in reversed(casts):  # 逆序插到最前，保持原相对顺序
    g.node.insert(0, c)

onnx.checker.check_model(m)
onnx.save(m, dst)
print(f"patched {len(casts)} int64 inputs -> {dst}")
