"""RK3588 TTS 评测：声学 int8 (CPU ORT) + 声码器 int8/fp16 (NPU RKNN) —— R14 TTS 定稿脚本
用法: python3 eval_tts_npu.py
产物: 精度（mel cos / 音频 cos+SNR vs fp32 参考）、RTF、demo wav（~/rk_tts/demo_k.wav）
说明：声学模型时长预测器→mel 帧数动态（内容相关 362-475），RKNN 静态形状不适配 →
     声学走 CPU ORT int8（MatMul/Gemm 动态量化，mel cos 0.99997）；声码器为纯前馈静态图
     → 冻结输入 [1,80,768]（宿主零填充）上 NPU int8/fp16。输入 3D 无布局坑。
"""
import time

import numpy as np
import onnxruntime as ort
from rknnlite.api import RKNNLite

D = "/home/cat/rk_tts"
LMAX = 768
SR, HOP = 22050, 256

oa = ort.InferenceSession(f"{D}/matcha_acoustic_int8.onnx", providers=["CPUExecutionProvider"])
voc = {}
for mode in ("int8", "fp16"):
    m = RKNNLite()
    assert m.load_rknn(f"{D}/hifigan_f768_{mode}.rknn") == 0, mode
    assert m.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0, mode
    voc[mode] = m


def wav_save(path, audio):
    a = np.clip(audio, -1.0, 1.0)
    pcm = (a * 32767).astype("<i2")
    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())


print("== 声学 int8 (CPU ORT)：mel 精度 + RTF")
mel_int8 = {}
for k in range(5):
    x = np.load(f"{D}/x_{k:02d}.npy")
    t0 = time.perf_counter()
    mel = oa.run(None, {"x": x, "noise_scale": np.array([0.0], np.float32),
                        "length_scale": np.array([1.0], np.float32)})[0]
    dt = time.perf_counter() - t0
    ref = np.load(f"{D}/mel_{k:02d}.npy")  # 已含 768 padding 的 fp32 参考
    f = mel.shape[-1]
    r = ref[0, :, :f].astype(np.float64).ravel()
    o = mel[0].astype(np.float64).ravel()
    cos = float((o * r).sum() / ((np.linalg.norm(o) * np.linalg.norm(r)) + 1e-9))
    mel_int8[k] = (mel, dt)
    print(f"  phrase{k}: frames={f} mel_cos={cos:.5f} time={dt*1000:.0f}ms")

print("== 声码器 NPU：音频精度（vs fp32 声码器同 mel）+ RTF")
for mode in ("int8", "fp16"):
    tot = 0.0
    for k in range(5):
        mel768 = np.load(f"{D}/mel_{k:02d}.npy")
        t0 = time.perf_counter()
        au = np.asarray(voc[mode].inference(inputs=[mel768])[0]).reshape(-1)
        dt = time.perf_counter() - t0
        tot += dt
        ref = np.load(f"{D}/ref_audio_{k:02d}.npy").reshape(-1)
        a = au.astype(np.float64); b = ref.astype(np.float64)
        cos = float((a * b).sum() / ((np.linalg.norm(a) * np.linalg.norm(b)) + 1e-9))
        noise = b - a
        snr = float(10 * np.log10((b ** 2).sum() / ((noise ** 2).sum() + 1e-9)))
        if mode == "int8" and k == 0:
            wav_save(f"{D}/demo_voc_int8.wav", au)
        print(f"  {mode} phrase{k}: audio_cos={cos:.5f} SNR={snr:.1f}dB time={dt*1000:.0f}ms "
              f"RTF={dt/(LMAX*HOP/SR):.3f}")
    print(f"  {mode} 平均 {(tot/5)*1000:.0f}ms/句(768帧)")

print("== 端到端 demo（声学 int8 CPU → 声码器 int8 NPU）")
x = np.load(f"{D}/x_00.npy")
t0 = time.perf_counter()
mel = oa.run(None, {"x": x, "noise_scale": np.array([0.0], np.float32),
                    "length_scale": np.array([1.0], np.float32)})[0]
f = mel.shape[-1]
padded = np.zeros((1, 80, LMAX), np.float32)
padded[0, :, :min(f, LMAX)] = mel[0, :, :min(f, LMAX)]
au = np.asarray(voc["int8"].inference(inputs=[padded])[0]).reshape(-1)
el = time.perf_counter() - t0
n_true = f * HOP
wav_save(f"{D}/demo_e2e.wav", au[:n_true])
print(f"  mel_frames={f} 总耗时={el*1000:.0f}ms 音频时长={n_true/SR:.2f}s 实时率={el/(n_true/SR):.3f} -> demo_e2e.wav")
print("TTS-EVAL-DONE")
