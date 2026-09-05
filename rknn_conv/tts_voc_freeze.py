"""hifigan 声码器手术：冻结 N=1/L=768 + onnxsim + 校验；并生成声码器校准集（真实 mel）与参考
用法: python tts_voc_freeze.py <hifigan.onnx> <matcha原模型.onnx> <lexicon.txt> <tokens.txt> <outdir>
"""
import sys
import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto

VOC, ACU, LEX, TOK, OUTDIR = sys.argv[1:6]
LMAX = 768

# ── 冻结声码器输入 ──
m = onnx.load(VOC)
for i in m.graph.input:
    for d in i.type.tensor_type.shape.dim:
        if d.dim_param == "N":
            d.Clear(); d.dim_value = 1
        if d.dim_param == "L":
            d.Clear(); d.dim_value = LMAX
import onnxsim
ms, ok = onnxsim.simplify(m)
assert ok
onnx.save(ms, f"{OUTDIR}/hifigan_f768.onnx")
print("voc frozen nodes:", len(ms.graph.node))

# ── 真实短语 → 音素 id（lexicon + tokens）──
tok2id = {}
for line in open(TOK, encoding="utf-8"):
    p = line.rstrip("\n").split(" ")
    if len(p) >= 2:
        tok2id[p[0]] = int(p[-1])
lex = {}
for line in open(LEX, encoding="utf-8"):
    p = line.rstrip("\n").split(maxsplit=1)
    if len(p) == 2:
        lex[p[0]] = p[1].split()
PHRASES = ["好的 已经 打开 行车 记录", "现在 正在 录像 存储 剩余 百分 之 五十",
           "今天 星期 三 温度 二十 六 度", "请 查看 前方 画面", "系统 运行 正常"]
seqs = []
for ph in PHRASES:
    ids = []
    for w in ph.split():
        if w in lex:
            ids.extend(tok2id.get(t, 1) for t in lex[w])
    if ids:
        seqs.append(np.array(ids, np.int64))
print("音素序列数:", len(seqs), "长度:", [len(s) for s in seqs])

# ── 原声学模型 fp32 生成真实 mel（noise=0 确定性）──
oa = ort.InferenceSession(ACU, providers=["CPUExecutionProvider"])
ov0 = ort.InferenceSession(VOC, providers=["CPUExecutionProvider"])
ov1 = ort.InferenceSession(f"{OUTDIR}/hifigan_f768.onnx", providers=["CPUExecutionProvider"])
ns0 = np.array([0.0], np.float32); ls = np.array([1.0], np.float32)
import os
os.makedirs(f"{OUTDIR}/voc_calib", exist_ok=True)
calib_lines = []
rng = np.random.RandomState(1)
for k, s in enumerate(seqs):
    x = np.zeros((1, 64), np.int64)
    n = min(len(s), 64)
    x[0, :n] = s[:n]
    x[0, n:] = 1  # pad
    mel = oa.run(None, {"x": x, "x_length": np.array([64], np.int64),
                        "noise_scale": ns0, "length_scale": ls})[0]
    f = mel.shape[-1]
    padded = np.zeros((1, 80, LMAX), np.float32)
    padded[0, :, :min(f, LMAX)] = mel[0, :, :min(f, LMAX)]
    np.save(f"{OUTDIR}/voc_calib/mel_{k:02d}.npy", padded)
    calib_lines.append(f"{OUTDIR}/voc_calib/mel_{k:02d}.npy")
    # 参考音频（原声码器，未冻结版本，裁到同长）
    a0 = ov0.run(None, {"mel": padded})[0]
    np.save(f"{OUTDIR}/voc_calib/ref_audio_{k:02d}.npy", a0)
    # 冻结版校验（同输入应一致）
    a1 = ov1.run(None, {"mel": padded})[0]
    d = float(np.abs(a0.astype(np.float64) - a1.astype(np.float64)).max())
    print(f"mel_{k:02d}: frames={f} voc-frozen maxdiff={d:.5f} audio_len={a1.shape[-1]}")
open(f"{OUTDIR}/calib_voc_npy.txt", "w").write("\n".join(calib_lines) + "\n")
print("VOC-FREEZE-DONE")
