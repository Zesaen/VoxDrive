"""生成 RKNN INT8 校准集 + fp32 参考输出。

复刻 sherpa-onnx OnlineZipformerTransducerModel 的流式推理：
  x = 特征帧 [t*32 : t*32+39]（不足复制末帧补齐），hop=32
  状态 = 7 组 × 5 层全零初始化（见 online-zipformer-transducer-model.cc GetEncoderInitStates）

产出：
  rknn_conv/calib/enc_XXXX.npz   encoder 校准样本（36 输入，真实缓存状态）
  rknn_conv/calib/dec_XXXX.npz   decoder 校准样本（y=[[tok_prev, tok_cur]]）
  rknn_conv/calib/joi_XXXX.npz   joiner 校准样本（encoder_out/decoder_out 真实激活）
  rknn_conv/calib/ref_outs.npz   各校准步的 fp32 encoder_out（后续 RKNN 精度对比用）
"""
import os
import sys

import numpy as np
import onnxruntime as ort
import sherpa_onnx
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MD = os.path.join(ROOT, "rknn_conv", "models", "asr-zipformer-zh-en")
OUT = os.path.join(ROOT, "rknn_conv", "calib")
os.makedirs(OUT, exist_ok=True)

CHUNK, LOOKAHEAD, FEAT_DIM = 32, 7, 80
WIN = CHUNK + LOOKAHEAD  # 39
NUM_ENC_LAYERS = [2, 4, 3, 2, 4]
LEFT_CONTEXT = [64, 32, 16, 8, 32]
ATT_DIM, ENC_DIM, CNN_K = 192, 384, 31
SAVE_AT = {0, 4, 12, 25}  # 每条 wav 保存的块序号（含 t=0 零状态）

ENC = os.path.join(MD, "encoder-epoch-99-avg-1.onnx")


def load_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return (pcm.astype(np.float32) / 32768.0)


def features_of(idx):
    """读 Jetson 上生产同款 C++ FeatureExtractor 导出的特征（int32 T, int32 dim, T*dim f32）"""
    path = os.path.join(ROOT, "rknn_conv", "models", f"feat_{idx}.bin")
    with open(path, "rb") as f:
        t, dim = np.fromfile(f, dtype=np.int32, count=2)
        data = np.fromfile(f, dtype=np.float32, count=t * dim)
    return data.reshape(t, dim)


def init_states():
    st = {}
    for i, L in enumerate(NUM_ENC_LAYERS):
        st[f"cached_len_{i}"] = np.zeros((L, 1), dtype=np.int64)
        st[f"cached_avg_{i}"] = np.zeros((L, 1, ENC_DIM), dtype=np.float32)
        st[f"cached_key_{i}"] = np.zeros((L, LEFT_CONTEXT[i], 1, ATT_DIM), dtype=np.float32)
        st[f"cached_val_{i}"] = np.zeros((L, LEFT_CONTEXT[i], 1, ATT_DIM // 2), dtype=np.float32)
        st[f"cached_val2_{i}"] = np.zeros((L, LEFT_CONTEXT[i], 1, ATT_DIM // 2), dtype=np.float32)
        st[f"cached_conv1_{i}"] = np.zeros((L, 1, ENC_DIM, CNN_K - 1), dtype=np.float32)
        st[f"cached_conv2_{i}"] = np.zeros((L, 1, ENC_DIM, CNN_K - 1), dtype=np.float32)
    return st


sess = ort.InferenceSession(ENC, providers=["CPUExecutionProvider"])
in_names = [i.name for i in sess.get_inputs()]
ref_outs = {}
calib_count = 0

for wi in (0, 1, 2):
    feat = features_of(wi)
    T = feat.shape[0]
    print(f"feat_{wi}: {T} frames -> {(T + CHUNK - 1) // CHUNK} chunks")
    states = init_states()
    step = 0
    t = 0
    while t < T:
        win = feat[t : t + WIN]
        if win.shape[0] < WIN:  # 末块复制末帧补齐
            pad = np.repeat(win[-1:], WIN - win.shape[0], axis=0)
            win = np.concatenate([win, pad], axis=0)
        feeds = {"x": win[np.newaxis].astype(np.float32)}
        feeds.update({k: v for k, v in states.items()})
        outs = sess.run(None, feeds)
        enc_out = outs[0]
        new_states = dict(zip(in_names[1:], outs[1:]))
        if step in SAVE_AT:
            np.savez(os.path.join(OUT, f"enc_{wi:02d}_{step:03d}.npz"),
                     **feeds, encoder_out=enc_out)
            ref_outs[f"{wi}_{step}"] = enc_out
            calib_count += 1
        states = new_states
        t += CHUNK
        step += 1

np.savez(os.path.join(OUT, "ref_outs.npz"), **ref_outs)
print(f"encoder calib: {calib_count} samples")

# decoder 校准：上下文两 token（blank=0 与随机真实 id 覆盖）
rng = np.random.default_rng(7)
for j in range(32):
    a, b = rng.integers(1, 5000, size=2)
    np.savez(os.path.join(OUT, f"dec_{j:03d}.npz"), y=np.array([[a, b]], dtype=np.int64))
# joiner 校准：用上面真实 encoder_out + 随机 decoder_out 量级激活
for j, key in enumerate(list(ref_outs)[:16]):
    eo = ref_outs[key][0]           # [frames, 512] 取一帧
    deo = np.tanh(rng.standard_normal((1, 512), dtype=np.float32)) * 2
    np.savez(os.path.join(OUT, f"joi_{j:03d}.npz"),
             encoder_out=eo[np.newaxis, 0], decoder_out=deo)
print("decoder/joiner calib done")
