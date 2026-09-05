"""把 calib/*.npz 转成 toolkit2 2.x 的 npy 数据集布局：
  calib_npy/<sample>/<input_name>.npy  +  calib_<model>_npy.txt（每行一个样本的全部输入路径）
decoder 的 y 改名 y_f 并转 float（与 decoder_final.onnx 的 float 输入一致）
"""
import glob
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "calib")
DST = os.path.join(HERE, "calib_npy")
BOARD = "/home/nvidia/rknn_conv"
os.makedirs(DST, exist_ok=True)


def dump(npz_path, prefix, rename_float=()):
    d = np.load(npz_path)
    name = os.path.splitext(os.path.basename(npz_path))[0]
    sdir = os.path.join(DST, name)
    os.makedirs(sdir, exist_ok=True)
    lines = []
    for k in d:
        if k == "encoder_out":
            continue  # 参考输出，非模型输入
        a = d[k]
        if k in rename_float:
            a = a.astype(np.float32)
            k = rename_float[rename_float.index(k)]  # noqa
        out_k = "y_f" if k == "y" else k
        np.save(os.path.join(sdir, f"{out_k}.npy"), a)
        lines.append(f"{BOARD}/calib_npy/{name}/{out_k}.npy")
    return " ".join(lines)


groups = {"encoder": sorted(glob.glob(SRC + "/enc_*.npz")),
          "decoder": sorted(glob.glob(SRC + "/dec_*.npz")),
          "joiner": sorted(glob.glob(SRC + "/joi_*.npz"))}
for model, files in groups.items():
    txt = []
    for f in files:
        txt.append(dump(f, model, rename_float=("y",)))
    with open(os.path.join(HERE, f"calib_{model}_npy.txt"), "w") as fh:
        fh.write("\n".join(txt) + "\n")
    print(model, len(files), "samples")
