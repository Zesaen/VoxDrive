"""微型对照模型生成：tiny(MatMul) / tiny_eq(Equal+Cast+MatMul)"""
import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

W = np.random.randn(4, 4).astype(np.float32)
m = helper.make_model(
    graph=helper.make_graph(
        [helper.make_node("MatMul", ["x", "W"], ["y"])], "tiny",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4])],
        initializer=[numpy_helper.from_array(W, "W")]))
m.ir_version = 8
onnx.save(m, "rknn_conv/models/tiny.onnx")

# 带 Equal/Cast 的版本（模拟 one-hot 头）
R = np.arange(50, dtype=np.float32)
m2 = helper.make_model(
    graph=helper.make_graph(
        [helper.make_node("Equal", ["x", "R"], ["eq"]),
         helper.make_node("Cast", ["eq"], ["oh"], to=TensorProto.FLOAT),
         helper.make_node("MatMul", ["oh", "W"], ["y"])], "tiny_eq",
        [helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 2])],
        [helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2, 4])],
        initializer=[numpy_helper.from_array(W, "W"),
                     numpy_helper.from_array(R, "R")]))
m2.ir_version = 8
onnx.save(m2, "rknn_conv/models/tiny_eq.onnx")
print("tiny + tiny_eq saved")
