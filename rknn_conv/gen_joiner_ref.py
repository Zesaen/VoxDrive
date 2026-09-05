"""生成 joiner 对照数据：8 帧 enc(bit-identical) + dec fp32 → ORT logits 参考"""
import numpy as np
import onnxruntime as ort

MD = "rknn_conv/models/asr-zipformer-zh-en"
enc_frames = np.load("rknn_conv/pc_chunks.npy")[:8]  # bit-identical to RK dump
dec = ort.InferenceSession(f"{MD}/decoder-epoch-99-avg-1.onnx", providers=["CPUExecutionProvider"])
joi = ort.InferenceSession(f"{MD}/joiner-epoch-99-avg-1.onnx", providers=["CPUExecutionProvider"])
d = dec.run(None, {"y": np.array([[0, 0]], dtype=np.int64)})[0]
logits = np.stack([joi.run(None, {"encoder_out": f[np.newaxis],
                                  "decoder_out": d.reshape(1, 512)})[0].reshape(-1)
                   for f in enc_frames])
np.savez("rknn_conv/joiner_ref.npz", enc=enc_frames, dec=d.reshape(1, 512), logits=logits)
print("ref logits", logits.shape, "argmax per frame:", [int(x) for x in logits.argmax(1)])
