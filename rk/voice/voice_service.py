#!/usr/bin/env python3
"""voice_service — RK3588 语音推理服务（R14 线上部署件）

ASR：PULL :voice.asr_pull(6711) 收 Jetson mic_stream 的 PCM（float32 16k mono）
     → kaldi-native-fbank（与 sherpa-onnx 同款前端）→ zipformer 流式编码（NPU core0）
     → transducer 贪心解码（decoder core1 + joiner core2）→ 终点检测
     → REQ Jetson intent_router:6666 发文本 + REQ tts_block:6677 防回灌
     （对外行为与原 Jetson sherpa-onnx-microphone 二进制一致，仅推理位置换到 RK NPU）
TTS：REP :voice.tts_rep(6720) 收 {"text": ...}
     → 数字归一化 + jieba 分词 + lexicon 音素映射 → matcha 声学 int8（CPU ORT）
     → hifigan 声码器 fp16（NPU）→ {"ok":true,"sr":22050,"pcm":base64(int16)}

**RKNN 喂数铁律（R14 实测定案，见 AGENTS.md §6）**：
  4D 缓存输入必须 transpose(0,2,3,1) 后 contiguous 喂入；输出恒等读回；输入顺序按
  encoder_input_order.txt（RKNN 转换后按 dtype/名字重排，≠ ONNX 序）。
"""
import base64
import json
import os
import signal
import sys
import threading
import time

import numpy as np
import zmq

# ---------------------------------------------------------------------------
# conf（jetson/config/voxdrive.rk.conf，板根仓库布局 jetson/config/ 下）
# ---------------------------------------------------------------------------

_CONF_CACHE = {}


def _load_conf():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (
        os.environ.get("VOX_CONF"),
        os.path.join(here, "..", "..", "jetson", "config", "voxdrive.rk.conf"),
        os.path.expanduser("~/Desktop/VoxDrive/jetson/config/voxdrive.rk.conf"),
    ):
        if cand and os.path.isfile(cand):
            for line in open(cand, encoding="utf-8"):
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    _CONF_CACHE[k.strip()] = v.strip()
            return
    print("[voice] WARN: 未找到 voxdrive.rk.conf，全用默认值")


def conf(key, default):
    return _CONF_CACHE.get(key, default)


# ---------------------------------------------------------------------------
# 模型常量（与 rknn_conv 工具链一致）
# ---------------------------------------------------------------------------
CHUNK, WIN = 32, 39          # 流式窗 39 帧（7 lookahead），步进 32
NUM_LAYERS = [2, 4, 3, 2, 4]
LEFT_CTX = [64, 32, 16, 8, 32]
ATT, ENC, CNN_K = 192, 384, 31
T4 = (0, 2, 3, 1)            # 4D 输入布局修复
LMAX, VMEL = 64, 768         # TTS：token 定长 / 声码器 mel 帧定长
SR_TTS, HOP = 22050, 256

stop_flag = False


def log(tag, msg):
    print(f"[{time.strftime('%H:%M:%S')}] [{tag}] {msg}", flush=True)


# ---------------------------------------------------------------------------
# ASR 推理器
# ---------------------------------------------------------------------------
class AsrEngine:
    def __init__(self, model_dir):
        from rknnlite.api import RKNNLite
        # 与生产 sherpa-onnx FeatureExtractor 逐位一致（feat_0.bin 对拍 maxabs=0 验证）：
        # snip_edges=False + mel 20..-400Hz 是 sherpa 默认、knf 默认不同，必须显式设
        from kaldi_native_fbank import OnlineFbank, FbankOptions

        opt = FbankOptions()
        opt.frame_opts.samp_freq = 16000
        opt.frame_opts.dither = 0
        opt.frame_opts.snip_edges = False
        opt.mel_opts.num_bins = 80
        opt.mel_opts.low_freq = 20
        opt.mel_opts.high_freq = -400
        self._fbank_cls = OnlineFbank
        self._fbank_opt = opt

        self.enc = RKNNLite()
        assert self.enc.load_rknn(f"{model_dir}/encoder_fp16.rknn") == 0
        assert self.enc.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0
        self.dec = RKNNLite()
        assert self.dec.load_rknn(f"{model_dir}/decoder_fp16.rknn") == 0
        assert self.dec.init_runtime(core_mask=RKNNLite.NPU_CORE_1) == 0
        self.joi = RKNNLite()
        assert self.joi.load_rknn(f"{model_dir}/joiner_fp16.rknn") == 0
        assert self.joi.init_runtime(core_mask=RKNNLite.NPU_CORE_2) == 0

        self.order = [l.strip() for l in open(f"{model_dir}/encoder_input_order.txt") if l.strip()]
        assert len(self.order) == 36

        self.id2tok = {}
        for line in open(f"{model_dir}/tokens.txt", encoding="utf-8"):
            p = line.rstrip("\n").split(" ")
            if len(p) >= 2:
                self.id2tok[int(p[-1])] = p[0]

    def reset(self):
        self.fbank = self._fbank_cls(self._fbank_opt)
        self.states = {}
        for i, L in enumerate(NUM_LAYERS):
            self.states[f"cached_len_{i}"] = np.zeros((L, 1), dtype=np.int64)
            self.states[f"cached_avg_{i}"] = np.zeros((L, 1, ENC), dtype=np.float32)
            self.states[f"cached_key_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT), dtype=np.float32)
            self.states[f"cached_val_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
            self.states[f"cached_val2_{i}"] = np.zeros((L, LEFT_CTX[i], 1, ATT // 2), dtype=np.float32)
            self.states[f"cached_conv1_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
            self.states[f"cached_conv2_{i}"] = np.zeros((L, 1, ENC, CNN_K - 1), dtype=np.float32)
        self.hyp = []
        self.t = 0                 # 已消费的特征帧指针
        self.last_emit_frame = 0   # 最近一次出非 blank token 的帧（终点检测用）

    def accept_pcm(self, pcm_f32):
        self.fbank.accept_waveform(16000, pcm_f32)

    def decode_available(self):
        """消费到 nframes-WIN+1（保证整窗）；返回 (新增 token 数, 已解码总帧数)"""
        n = self.fbank.num_frames_ready
        new_tok = 0
        while self.t + WIN <= n:
            win = np.stack([np.asarray(self.fbank.get_frame(i), dtype=np.float32)
                            for i in range(self.t, self.t + WIN)])
            outs = self.enc.inference(inputs=self._feeds(win))
            enc_out = np.asarray(outs[0]).reshape(-1, 512).astype(np.float32)
            self.states = dict(zip(self.order[1:], [np.asarray(o) for o in outs[1:]]))
            self.t += CHUNK
            for fi in range(enc_out.shape[0]):
                if self._decode_frame(enc_out[fi]):
                    new_tok += 1
                    self.last_emit_frame = self.t
        return new_tok, self.t

    def flush_tail(self):
        """终点后补零冲出尾窗剩余解码（Eval 同款 repeat-pad 兜底）"""
        n = self.fbank.num_frames_ready
        while self.t < n:
            idx = [min(i, n - 1) for i in range(self.t, self.t + WIN)]
            win = np.stack([np.asarray(self.fbank.get_frame(i), dtype=np.float32) for i in idx])
            outs = self.enc.inference(inputs=self._feeds(win))
            enc_out = np.asarray(outs[0]).reshape(-1, 512).astype(np.float32)
            self.states = dict(zip(self.order[1:], [np.asarray(o) for o in outs[1:]]))
            self.t += CHUNK
            for fi in range(enc_out.shape[0]):
                self._decode_frame(enc_out[fi])

    def _feeds(self, win):
        feeds = [win[np.newaxis].astype(np.float32)]
        for k in self.order[1:]:
            a = self.states[k]
            feeds.append(np.ascontiguousarray(np.transpose(a, T4)) if a.ndim == 4 else a)
        return feeds

    def _decode_frame(self, frame):
        for _ in range(10):
            ctx = np.array([[self.hyp[-2] if len(self.hyp) >= 2 else 0,
                             self.hyp[-1] if len(self.hyp) >= 1 else 0]], dtype=np.int32)
            d = np.asarray(self.dec.inference(inputs=[ctx])[0]).reshape(1, 512).astype(np.float32)
            logits = np.asarray(self.joi.inference(inputs=[frame[np.newaxis], d])[0]).reshape(-1)
            tok = int(np.argmax(logits))
            if tok == 0:
                return False
            self.hyp.append(tok)
        return True

    def text(self):
        return "".join(self.id2tok.get(x, "") for x in self.hyp if x != 0).replace("▁", " ").strip()


# ---------------------------------------------------------------------------
# TTS 推理器
# ---------------------------------------------------------------------------
_NUM_ZH = "零一二三四五六七八九"


def _num2zh(s):
    """阿拉伯数字串 → 汉字（整数/小数/%，TTS 前置归一化，基本口径）"""
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "%":
            out.append("百分之")
            i += 1
        elif c.isdigit():
            j = i
            while j < len(s) and (s[j].isdigit() or s[j] == "."):
                j += 1
            num = s[i:j]
            if num.endswith("."):
                num = num[:-1]
            try:
                if "." in num:
                    ip, dp = num.split(".", 1)
                    zh = ("".join(_NUM_ZH[int(ch)] for ch in ip) if ip else "零") + "点" + \
                         "".join(_NUM_ZH[int(ch)] for ch in dp)
                else:
                    n = int(num)
                    if n == 0:
                        zh = "零"
                    else:
                        zh = ""
                        for unit, name in ((100000000, "亿"), (10000, "万"), (1, "")):
                            q, n = divmod(n, unit)
                            if q:
                                s0 = str(q)
                                if len(s0) >= 2 and s0[0] == "1" and unit != 1 and len(s0) == 2:
                                    zh += "十" + "".join(_NUM_ZH[int(x)] for x in s0[1:])
                                else:
                                    zh += "".join(_NUM_ZH[int(x)] for x in s0)
                                zh += name
                        zh = zh or "零"
                out.append(zh)
            except ValueError:
                out.append(num)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


class TtsEngine:
    def __init__(self, model_dir):
        import onnxruntime as ort
        from rknnlite.api import RKNNLite

        self.acoustic = ort.InferenceSession(f"{model_dir}/matcha_acoustic_int8.onnx",
                                             providers=["CPUExecutionProvider"])
        self.voc = RKNNLite()
        assert self.voc.load_rknn(f"{model_dir}/hifigan_f768_fp16.rknn") == 0
        assert self.voc.init_runtime(core_mask=RKNNLite.NPU_CORE_0) == 0

        self.lex = {}
        for line in open(f"{model_dir}/lexicon.txt", encoding="utf-8"):
            p = line.rstrip("\n").split(maxsplit=1)
            if len(p) == 2:
                self.lex[p[0]] = p[1].split()
        self.tok2id = {}
        for line in open(f"{model_dir}/tokens.txt", encoding="utf-8"):
            p = line.rstrip("\n").split(" ")
            if len(p) >= 2:
                self.tok2id[p[0]] = int(p[-1])
        import jieba
        jieba.initialize()
        self.jieba = jieba
        log("tts", f"lexicon={len(self.lex)} 词, tokens={len(self.tok2id)}")

    def _text2ids(self, text):
        ids = []
        for w in self.jieba.cut(text, HMM=False):
            if w.strip() and w in self.lex:
                ids.extend(self.tok2id.get(t, 1) for t in self.lex[w])
            else:
                for ch in w:
                    if ch in self.lex:
                        ids.extend(self.tok2id.get(t, 1) for t in self.lex[ch])
        x = np.zeros((1, LMAX), np.int32)
        n = min(len(ids), LMAX)
        x[0, :n] = ids[:n]
        x[0, n:] = 1  # pad 音素（尾部近静音）
        return x

    def synthesize(self, text):
        t0 = time.perf_counter()
        x = self._text2ids(_num2zh(text.strip()))
        mel = self.acoustic.run(None, {"x": x, "noise_scale": np.array([0.0], np.float32),
                                       "length_scale": np.array([1.0], np.float32)})[0]
        f = min(mel.shape[-1], VMEL)
        padded = np.zeros((1, 80, VMEL), np.float32)
        padded[0, :, :f] = mel[0, :, :f]
        audio = np.asarray(self.voc.inference(inputs=[padded])[0]).reshape(-1).astype(np.float32)
        audio = audio[:f * HOP]
        # 能量裁剪：尾部 pad 音素近静音，留 200ms 余量
        mag = np.abs(audio)
        nz = np.where(mag > 0.01)[0]
        if nz.size:
            audio = audio[:min(len(audio), nz[-1] + int(0.2 * SR_TTS))]
        pcm = (np.clip(audio, -1.0, 1.0) * 32767).astype("<i2")
        ms = (time.perf_counter() - t0) * 1000
        return pcm, ms


# ---------------------------------------------------------------------------
# 主循环
# ---------------------------------------------------------------------------

def asr_worker(ctx, engine):
    pull = ctx.socket(zmq.PULL)
    pull.bind(f"tcp://*:{conf('voice.asr_pull', '6711')}")
    log("asr", f"PULL :{conf('voice.asr_pull', '6711')} 就绪，等 Jetson PCM")

    jhost = conf("jetson.ip", "192.168.137.190")
    router_ep = f"tcp://{jhost}:{conf('port.intent_router', '6666')}"
    block_ep = f"tcp://{jhost}:{conf('port.tts_block', '6677')}"
    endpoint_sil = float(conf("voice.endpoint_sil_s", "0.7"))
    max_utt_s = float(conf("voice.max_utt_s", "30"))
    vad_rms = float(conf("voice.vad_rms", "0.006"))

    def dispatch(text):
        """终点后转发（gate→router）。独立线程：router 含 LLM 可达秒级，不能阻塞采音循环"""
        try:
            gate = ctx.socket(zmq.REQ)
            gate.setsockopt(zmq.RCVTIMEO, 1000)
            gate.setsockopt(zmq.LINGER, 0)
            gate.connect(block_ep)
            gate.send_string("block")
            gate.recv_string()
            gate.close()
        except zmq.ZMQError:
            pass
        try:
            r = ctx.socket(zmq.REQ)
            r.setsockopt(zmq.RCVTIMEO, 8000)
            r.setsockopt(zmq.LINGER, 0)
            r.connect(router_ep)
            r.send_string(text)
            reply = r.recv_string()
            log("asr", f"router: {reply[:120]}")
            r.close()
        except zmq.ZMQError as e:
            log("asr", f"router REQ 失败: {e}")

    engine.reset()
    utt_start = time.time()
    last_voice = time.time()
    while not stop_flag:
        # 超时也落到终点判定：流停推后（mic 断/尾静音）最后一utterance必须能出终点
        pcm = None
        try:
            pull.setsockopt(zmq.RCVTIMEO, 200)
            pcm = np.frombuffer(pull.recv(), dtype=np.float32)
        except zmq.Again:
            pass
        now = time.time()
        if pcm is not None:
            engine.accept_pcm(pcm)
            engine.decode_available()
            # 能量 VAD 定终点：不能按 token 输出间隔判静音——整词单 token 的语言
            # （英文 BPE）正常语音间隔可超 1s，会造成句中假终点+冷启动碎片
            if float(np.sqrt(np.mean(pcm * pcm))) > vad_rms:
                last_voice = now
        voiced = len(engine.hyp) > 0
        if not voiced and now - utt_start > 15:  # 无人说话也重置，防缓冲无限增长
            engine.reset()
            utt_start = now
            last_voice = now
        elif voiced and (now - last_voice >= endpoint_sil or now - utt_start > max_utt_s):
            engine.flush_tail()
            text = engine.text()
            if text:
                log("asr", f"final: {text!r}")
                threading.Thread(target=dispatch, args=(text,), daemon=True).start()
            engine.reset()
            utt_start = now


def tts_worker(ctx, engine):
    rep = ctx.socket(zmq.REP)
    rep.bind(f"tcp://*:{conf('voice.tts_rep', '6720')}")
    log("tts", f"REP :{conf('voice.tts_rep', '6720')} 就绪")
    while not stop_flag:
        try:
            rep.setsockopt(zmq.RCVTIMEO, 200)
            req = json.loads(rep.recv_string())
        except zmq.Again:
            continue
        except json.JSONDecodeError:
            rep.send_string(json.dumps({"ok": False, "err": "bad json"}))
            continue
        text = str(req.get("text", "")).strip()
        if not text:
            rep.send_string(json.dumps({"ok": False, "err": "empty text"}))
            continue
        try:
            pcm, ms = engine.synthesize(text)
            rep.send_string(json.dumps({
                "ok": True, "sr": SR_TTS, "frames": int(pcm.size),
                "ms": int(ms), "pcm": base64.b64encode(pcm.tobytes()).decode()}))
            log("tts", f"synth {text[:24]!r} → {pcm.size} 样本 ({ms:.0f}ms)")
        except Exception as e:
            log("tts", f"synth 失败: {e}")
            rep.send_string(json.dumps({"ok": False, "err": str(e)}))


def main():
    _load_conf()
    asr_dir = conf("voice.asr_dir", os.path.expanduser("~/rk_asr"))
    tts_dir = conf("voice.tts_dir", os.path.expanduser("~/rk_tts"))

    signal.signal(signal.SIGINT, lambda *_: globals().__setitem__("stop_flag", True))

    asr = AsrEngine(asr_dir)
    log("asr", "三模型 NPU 加载完成 (enc core0 / dec core1 / joi core2)")
    tts = TtsEngine(tts_dir)
    log("tts", "声学 int8(CPU) + 声码器 fp16(NPU) 加载完成")

    ctx = zmq.Context()
    threading.Thread(target=tts_worker, args=(ctx, tts), daemon=True).start()
    asr_worker(ctx, asr)
    log("voice", "退出")


if __name__ == "__main__":
    main()
