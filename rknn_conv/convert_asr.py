"""RKNN INT8 转换（在 Jetson 上跑，PYTHONPATH=~/rknn_conv/pylib）
用法: python3 convert_asr.py <encoder|decoder|joiner> [quantized_algorithm]
产物: out/<model>_int8.rknn
"""
import os
import sys

from rknn.api import RKNN

MODEL = sys.argv[1]
ALGO = sys.argv[3] if len(sys.argv) > 3 else "normal"
BASE = os.path.dirname(os.path.abspath(__file__))
ONNX = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BASE, "models", f"{MODEL}_b1.onnx")
DATASET = os.path.join(BASE, f"calib_{MODEL}_npy.txt")
OUT = os.path.join(BASE, "out")
os.makedirs(OUT, exist_ok=True)

import os
rknn = RKNN(verbose=os.environ.get("RKNN_VERBOSE") == "1")
print(f"[{MODEL}] config (algo={ALGO})")
if rknn.config(
    target_platform="rk3588",
    quantized_dtype="w8a8",            # INT8 权重+激活
    quantized_algorithm=ALGO,          # normal | kl_divergence | mmse
    quantized_method="channel",
    optimization_level=int(os.environ.get("RKNN_OPT", "3")),
) != 0:
    sys.exit("config fail")
print(f"[{MODEL}] load_onnx {ONNX}")
if rknn.load_onnx(model=ONNX) != 0:
    sys.exit("load fail")
print(f"[{MODEL}] build(dataset={DATASET})")
if rknn.build(do_quantization=True, dataset=DATASET) != 0:
    sys.exit("build fail")
tag = os.path.splitext(os.path.basename(ONNX))[0]
out_path = os.path.join(OUT, f"{MODEL}_{tag}_int8.rknn")
if rknn.export_rknn(out_path) != 0:
    sys.exit("export fail")
print(f"[{MODEL}] OK -> {out_path}")
