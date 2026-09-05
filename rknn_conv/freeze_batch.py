"""把 ONNX 里所有 'N'（batch）维度冻结为 1——RKNN 只接受静态 shape。
用法: python freeze_batch.py <in.onnx> <out.onnx>
"""
import sys

import onnx

src, dst = sys.argv[1], sys.argv[2]
m = onnx.load(src)

n = 0
for g in [m.graph]:
    for i in g.input:
        for d in i.type.tensor_type.shape.dim:
            if d.dim_param == "N":
                d.dim_param = ""
                d.dim_value = 1
                n += 1
    for o in g.output:
        for d in o.type.tensor_type.shape.dim:
            if d.dim_param == "N":
                d.dim_param = ""
                d.dim_value = 1
                n += 1
onnx.checker.check_model(m)
onnx.save(m, dst)
print(f"frozen {n} dims -> {dst}")
