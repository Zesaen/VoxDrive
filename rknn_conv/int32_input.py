"""把 int64 图输入改为 int32 并删除直接消费它的 Cast(to=INT64) 无操作节点。
适用: decoder (y→Cast→Gather)。encoder 的 cached_len 若下游有 int64 运算则不适用。
用法: python int32_input.py <in.onnx> <out.onnx>
"""
import sys

import onnx
from onnx import TensorProto, helper

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)
g = m.graph

removed = 0
new_inputs = []
for i in g.input:
    tt = i.type.tensor_type
    if tt.elem_type == TensorProto.INT64:
        # 找到唯一消费它的 Cast(to=INT64)，删除并把下游改接输入名
        consumers = [n for n in g.node if i.name in n.input]
        cast_nodes = [n for n in consumers
                      if n.op_type == "Cast"
                      and any(a.name == "to" and a.i == TensorProto.INT64 for a in n.attribute)]
        if len(consumers) == 1 and len(cast_nodes) == 1:
            cast = cast_nodes[0]
            out_name = cast.output[0]
            for n in g.node:
                for k, inp in enumerate(n.input):
                    if inp == out_name:
                        n.input[k] = i.name
            g.node.remove(cast)
            removed += 1
            dims = [(d.dim_value if d.dim_value else 1) for d in tt.shape.dim]
            new_inputs.append(helper.make_tensor_value_info(
                i.name, TensorProto.INT32, dims))
            continue
    new_inputs.append(i)

del g.input[:]
g.input.extend(new_inputs)
onnx.checker.check_model(m)
onnx.save(m, dst)
print(f"removed {removed} no-op casts, int32 inputs -> {dst}")
