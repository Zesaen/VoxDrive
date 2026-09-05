"""验证 one-hot 改造图与原图输出等价"""
import numpy as np
import onnxruntime as ort

a = ort.InferenceSession(
    "rknn_conv/models/asr-zipformer-zh-en/decoder-epoch-99-avg-1.onnx",
    providers=["CPUExecutionProvider"]).run(
        None, {"y": np.array([[3, 7]], dtype=np.int64)})[0]
b = ort.InferenceSession(
    "rknn_conv/models/decoder_final.onnx",
    providers=["CPUExecutionProvider"]).run(
        None, {"y_f": np.array([[3.0, 7.0]], dtype=np.float32)})[0]
print("orig vs onehot maxdiff:", float(np.abs(a - b).max()))
