"""decoder 精度分析 + 混合量化（toolkit2 2.3.2 流程，在 Jetson 上跑）
step1(proposal=True 自动建议敏感层) → 精度分析报告 → 改 cfg 标 float16 → step2 → export
用法: python3 hybrid_decoder.py models/decoder_final.onnx calib_decoder_npy.txt
"""
import os
import sys

from rknn.api import RKNN

BASE = os.path.dirname(os.path.abspath(__file__))
ONNX = sys.argv[1]
DATASET = sys.argv[2]
os.chdir(BASE)

rknn = RKNN(verbose=False)
rknn.config(target_platform="rk3588", quantized_dtype="w8a8",
            quantized_algorithm="normal", quantized_method="channel",
            optimization_level=3)
assert rknn.load_onnx(model=ONNX) == 0

print("== step1 (proposal) ==")
ret = rknn.hybrid_quantization_step1(dataset=DATASET, proposal=False)
assert ret == 0, f"step1 fail {ret}"
base = ONNX.split("/")[-1].replace(".onnx", "")
cfg = f"{base}.quantization.cfg"
print("generated:", [f for f in os.listdir(".") if base in f])

print("== 精度分析（INT8 逐层 cosine）==")
rknn2 = RKNN(verbose=False)
rknn2.config(target_platform="rk3588", quantized_dtype="w8a8",
             quantized_algorithm="normal", quantized_method="channel",
             optimization_level=3)
rknn2.load_onnx(model=ONNX)
rknn2.build(do_quantization=True, dataset=DATASET)
ret = rknn2.accuracy_analysis(inputs=DATASET, output_dir="./snap_decoder",
                              target=None)
print("accuracy_analysis ret:", ret)

# 展示 step1 生成的 cfg（含 proposal 标注）
print("== cfg（float16 行）==")
txt = open(cfg, encoding="utf-8").read()
fp16_lines = [l for l in txt.splitlines() if "float16" in l]
print("\n".join(fp16_lines[:15]) or "(无 float16 标注，尝试手动补 one-hot 层)")
if not fp16_lines:
    new_lines = []
    for l in txt.splitlines():
        if any(t in l for t in ("oh_eq", "oh_cast", "oh_reshape", "oh_mm")):
            import re
            l = re.sub(r"(:\s*)\w+", r"\1float16", l, count=1)
        new_lines.append(l)
    open(cfg, "w", encoding="utf-8").write("\n".join(new_lines) + "\n")
    print("已手动标注 one-hot 层为 float16")

print("== step2 ==")
ret = rknn.hybrid_quantization_step2(
    model_input=f"{base}.model", data_input=f"{base}.data",
    model_quantization_cfg=cfg)
assert ret == 0, f"step2 fail {ret}"
out = "out/decoder_hybrid.rknn"
os.makedirs("out", exist_ok=True)
rknn.export_rknn(out)
print("OK ->", out)
