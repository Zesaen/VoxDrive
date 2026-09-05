"""matcha 声学模型 ONNX 手术 v3：x_length 烤成常量 + x 转 int32 + 冻结 N=1/L=Lmax + onnxsim
用法: python tts_freeze.py <in.onnx> <out.onnx> [Lmax]

设计（全部来自 ASR 转换的已验证经验）：
- x (token ids) int64→int32：RKNN 只跟踪 float 通道，int64 输入当常数；int32 直入 Gather 可行
- x_length 不再作图输入，而是注入 int64 Constant [Lmax]：位置/掩码链全变编译期常量，
  onnxsim 整链折叠——绕开 int64/int32 混型（Unsqueeze axes/Reshape shape 等规范要求 int64）
- 运行时 token 序列一律右填充到 Lmax，尾部音频由宿主按 pad 音素近似静音做能量裁剪
"""
import sys

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

IN, OUT = sys.argv[1], sys.argv[2]
LMAX = int(sys.argv[3]) if len(sys.argv) > 3 else 64

m = onnx.load(IN)
g = m.graph

# ── 1) x_length 输入 → int64 Constant 注入 ──
xl = next(i for i in g.input if i.name == "x_length")
xl_len = xl.type.tensor_type.shape.dim[0].dim_value or 1
const_out = "x_length_baked"
cn = helper.make_node(
    "Constant", [], [const_out],
    value=numpy_helper.from_array(np.full([xl_len], LMAX, dtype=np.int64), "xl_baked"),
    name="x_length_baked_node")
g.node.insert(0, cn)
for n in g.node:
    n.input[:] = [const_out if t == "x_length" else t for t in n.input]
g.input.remove(xl)
print(f"x_length baked = {LMAX}")

# ── 2) x 输入 int64→int32 + 冻结 N/L ──
for i in g.input:
    if i.name == "x":
        i.type.tensor_type.elem_type = TensorProto.INT32
        print("x -> int32")
    for d in i.type.tensor_type.shape.dim:
        if d.dim_param == "N":
            d.Clear(); d.dim_value = 1
        if d.dim_param == "L":
            d.Clear(); d.dim_value = LMAX
for o in g.output:
    for d in o.type.tensor_type.shape.dim:
        if d.dim_param in ("N", "L"):
            d.Clear(); d.dim_value = 1 if d.dim_param == "N" else LMAX
print(f"frozen N=1 L={LMAX}")
onnx.save(m, OUT + ".stage1.onnx")

# ── 3) ORT 可载入性（混型会在此暴露）──
import onnxruntime as ort
try:
    ort.InferenceSession(OUT + ".stage1.onnx", providers=["CPUExecutionProvider"])
    print("stage1 ORT OK")
except Exception as e:
    print("stage1 load fail:", str(e)[:400]); sys.exit(1)

# ── 4) onnxsim + 终检 ──
import onnxsim
ms, ok = onnxsim.simplify(OUT + ".stage1.onnx")
assert ok
onnx.save(ms, OUT)
print("sim ok, nodes:", len(ms.graph.node), "->", OUT)

o1 = ort.InferenceSession(OUT, providers=["CPUExecutionProvider"])
o0 = ort.InferenceSession(IN, providers=["CPUExecutionProvider"])
rng = np.random.RandomState(0)
for xlen in (LMAX, 20):
    x = rng.randint(1, 300, size=(1, LMAX)).astype(np.int64)
    x[:, xlen:] = 1
    ns = np.array([0.667], np.float32); ls = np.array([1.0], np.float32)
    m0 = o0.run(None, {"x": x, "x_length": np.array([LMAX], np.int64), "noise_scale": ns, "length_scale": ls})[0]
    m1 = o1.run(None, {"x": x.astype(np.int32), "noise_scale": ns, "length_scale": ls})[0]
    d = float(np.abs(m0.astype(np.float64) - m1.astype(np.float64)).max())
    print(f"trial xlen={xlen} (feed x_length={LMAX}): mel orig={m0.shape} new={m1.shape} maxdiff={d:.6f}")
print("FREEZE-DONE")
