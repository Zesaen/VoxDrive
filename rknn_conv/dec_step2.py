"""decoder fp16 混合 step2 + 导出"""
from rknn.api import RKNN

rknn = RKNN(verbose=False)
rknn.config(target_platform="rk3588", quantized_dtype="w8a8",
            quantized_algorithm="normal", quantized_method="channel",
            optimization_level=3)
rknn.load_onnx(model="models/decoder_i32.onnx")
ret = rknn.hybrid_quantization_step2(
    model_input="decoder_i32.model",
    data_input="decoder_i32.data",
    model_quantization_cfg="decoder_i32.quantization.cfg")
print("step2:", ret)
assert ret == 0
rknn.export_rknn("out/decoder_fp16hybrid.rknn")
print("EXPORTED")
