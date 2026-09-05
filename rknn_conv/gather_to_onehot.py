"""把「Cast(y)→Gather(embedding)」替换为纯 float 数据流：Equal(y_f, range)→Cast(float)→MatMul。
RKNN 的 load 检查只跟踪 float 通道，整数索引链会被判“输出常数”。
用法: python gather_to_onehot.py <in.onnx> <out.onnx> <y_input_name> <cast_out_name> <gather_out_name>
"""
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

src, dst = sys.argv[1], sys.argv[2]
y_name, cast_out, gather_out = sys.argv[3], sys.argv[4], sys.argv[5]

m = onnx.load(src)
g = m.graph

cast_node = next(n for n in g.node if n.op_type == "Cast" and cast_out in n.input)
gather_node = next(n for n in g.node if n.op_type == "Gather" and gather_out in n.output)
w_name = gather_node.input[0]
W = numpy_helper.to_array(next(t for t in g.initializer if t.name == w_name))
V, D = W.shape
print(f"embedding [{V}, {D}]")

new_nodes = [
    helper.make_node("Equal", [y_name, "onehot_range"], ["onehot_eq"], name="oh_eq"),
    helper.make_node("Cast", ["onehot_eq"], ["onehot_f"], to=TensorProto.FLOAT, name="oh_cast"),
    helper.make_node("MatMul", ["onehot_f", w_name], [gather_out], name="oh_matmul"),
]
rng = np.arange(V, dtype=np.float32)
g.initializer.append(numpy_helper.from_array(rng.reshape(1, 1, V), "onehot_range"))

# 删除 Cast 与 Gather，插入新节点于原 Cast 位置
idx = list(g.node).index(cast_node)
g.node.remove(cast_node)
g.node.remove(gather_node)
for k, n in enumerate(new_nodes):
    g.node.insert(idx + k, n)

onnx.checker.check_model(m)
onnx.save(m, dst)
print(f"saved {dst}")
