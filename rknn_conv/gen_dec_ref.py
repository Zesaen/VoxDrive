"""生成 decoder fp32 参考输出（y_f=[3,7]）供 RK 板端对比"""
import numpy as np
import onnxruntime as ort

s = ort.InferenceSession("rknn_conv/models/decoder_final.onnx",
                         providers=["CPUExecutionProvider"])
o = s.run(None, {"y_f": np.array([[3.0, 7.0]], dtype=np.float32)})[0]
np.save("rknn_conv/calib/dec_ref_3_7.npy", o)
print("ref saved", o.shape)
