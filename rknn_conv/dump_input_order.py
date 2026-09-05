"""输出 encoder_sim 的图输入顺序（lite2 inference 按此序喂）"""
import onnx

m = onnx.load("rknn_conv/models/encoder_sim.onnx", load_external_data=False)
with open("rknn_conv/encoder_input_order.txt", "w") as f:
    for i in m.graph.input:
        f.write(i.name + "\n")
print("wrote rknn_conv/encoder_input_order.txt",
      len(m.graph.input), "inputs")
