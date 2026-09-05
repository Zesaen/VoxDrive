"""PC 侧 sherpa-onnx 基线转写（int8 三件套，贪心）——作为 RK NPU 管线的对照"""
import sys

import sherpa_onnx

import numpy as np
import wave
MD = "rknn_conv/models/asr-zipformer-zh-en"
wav = sys.argv[1] if len(sys.argv) > 1 else f"{MD}/test_wavs/0.wav"

rec = sherpa_onnx.OnlineRecognizer.from_transducer(
    encoder=f"{MD}/encoder-epoch-99-avg-1.int8.onnx",
    decoder=f"{MD}/decoder-epoch-99-avg-1.int8.onnx",
    joiner=f"{MD}/joiner-epoch-99-avg-1.int8.onnx",
    tokens=f"{MD}/tokens.txt",
    num_threads=2,
    decoding_method="greedy_search",
)
s = rec.create_stream()
with wave.open(wav, "rb") as w:
    pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
samples = pcm.astype(np.float32) / 32768.0
feed = 1600  # 0.1s
i = 0
while i < len(samples):
    s.accept_waveform(16000, samples[i:i + feed])
    while rec.is_ready(s):
        rec.decode_stream(s)
    i += feed
s.input_finished()
while rec.is_ready(s):
    rec.decode_stream(s)
print("BASELINE:", rec.get_result(s))
