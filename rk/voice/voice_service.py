#!/usr/bin/env python3
"""voice_service — RK3588 ASR 推理服务（R14 线上部署件）

ASR 输入两路（2026-09-06 起）：
  · 本板麦克风（默认）：PulseAudio parec 采集 16k s16le → 进程内喂数
    （麦克风物理在 RK 板，conf voice.mic_source 指定 pulse 源，空=禁用）
  · PULL :voice.asr_pull(6711)：外部注入（wav_push 验收工具 / Jetson mic_stream 保留通路）
     → kaldi-native-fbank（与 sherpa-onnx 同款前端）→ zipformer 流式编码（NPU core0）
     → transducer 贪心解码（decoder core1 + joiner core2）→ 能量 VAD 终点检测
     → REQ Jetson intent_router:6666 发文本 + REQ tts_block:6677 防回灌
TTS：由 rk/tts/tts_node（SummerTTS CPU 引擎）接管，REP :6720 协议。

**RKNN 喂数铁律（R14 实测定案，见 AGENTS.md §6）**：
  4D 缓存输入必须 transpose(0,2,3,1) 后 contiguous 喂入；输出恒等读回；输入顺序按
  encoder_input_order.txt（RKNN 转换后按 dtype/名字重排，≠ ONNX 序）。
"""
import json
import os
import queue
import signal
import subprocess
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
# 主循环
# ---------------------------------------------------------------------------

def mic_worker(source, q):
    """本板麦克风采集：parec 16k s16le mono → 队列（断流自动重启子进程）"""
    cmd = ["parec", "--format=s16le", "--rate=16000", "--channels=1",
           "--device", source]
    while not stop_flag:
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL)
        except FileNotFoundError:
            log("mic", "parec 不存在（pulseaudio-utils 未装），麦克风采集禁用")
            return
        log("mic", f"采集启动: {source}")
        while not stop_flag:
            data = p.stdout.read(1024)      # 512 样本 = 32ms
            if not data:
                break
            q.put(np.frombuffer(data, "<i2").astype(np.float32) / 32768.0)
        p.kill()
        p.wait()
        if not stop_flag:
            log("mic", "采集断流，3s 后重启")
            time.sleep(3)


def asr_worker(ctx, engine):
    pull = ctx.socket(zmq.PULL)
    pull.bind(f"tcp://*:{conf('voice.asr_pull', '6711')}")

    jhost = conf("jetson.ip", "192.168.137.190")
    router_ep = f"tcp://{jhost}:{conf('port.intent_router', '6666')}"
    block_ep = f"tcp://{jhost}:{conf('port.tts_block', '6677')}"
    endpoint_sil = float(conf("voice.endpoint_sil_s", "0.7"))
    max_utt_s = float(conf("voice.max_utt_s", "30"))
    vad_rms = float(conf("voice.vad_rms", "0.006"))

    # 本板麦克风（麦克风物理在 RK）：parec 线程喂数，与 PULL 注入共用解码通路
    mic_q = queue.Queue(maxsize=64)   # 满即丢——采音永远优先于积压
    mic_src = conf("voice.mic_source", "")
    if mic_src:
        threading.Thread(target=mic_worker, args=(mic_src, mic_q), daemon=True).start()
        log("asr", f"麦克风源: {mic_src}")
    log("asr", f"PULL :{conf('voice.asr_pull', '6711')} 就绪（外部注入通路）")

    # ── 播报期防回灌：音箱与麦克风是同一 USB 组合设备，播报声会被自家人脸识别 ──
    # 订阅 router tts_say（开播静音）与 tts play_end（结束后解除）；
    # play_end 是 Jetson 侧「合成+写入完成」事件，远早于本板 paplay 实时播完
    # （实测 12s 音频 play_end 2.9s 就到）——解除必须在 play_end 之后等麦克风
    # 真实静音 ≥voice.unmute_silence_s，否则长答复播到一半就收自己的声音级联查询。
    # play_end 丢失保护：静音超 30s 强制解除。静音期丢弃 PCM 并复位解码状态。
    ev = ctx.socket(zmq.SUB)
    ev.setsockopt_string(zmq.SUBSCRIBE, "")
    ev.setsockopt(zmq.LINGER, 0)
    ev.connect(f"tcp://{jhost}:{conf('port.intent_router_pub', '6671')}")
    ev.connect(f"tcp://{jhost}:{conf('port.tts_pub', '6678')}")
    muted = False
    muted_since = 0.0
    unmute_at = 0.0
    muted_voice_at = 0.0     # 静音期内最后一次听到能量（播报声/噪声）的时刻
    unmute_sil = float(conf("voice.unmute_silence_s", "1.0"))

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
        # 播报事件驱动静音（防回灌）
        now = time.time()
        while True:
            try:
                msg = ev.recv_string(zmq.NOBLOCK)
            except zmq.Again:
                break
            if '"tts_say"' in msg and not muted:
                muted, muted_since = True, now
                muted_voice_at = now
                engine.reset()
                log("asr", "播报开始，识别静音")
            elif msg.strip() == "play_end" and muted:  # tts_server 发裸字符串
                unmute_at = now + float(conf("voice.unmute_tail_s", "0.6"))
                log("asr", "播报写入完成，等静音 %.1fs 后解除" % (unmute_sil,))
        if muted:
            if (unmute_at and now >= unmute_at
                    and now - muted_voice_at >= unmute_sil):
                muted, unmute_at = False, 0.0
                utt_start = last_voice = now
                log("asr", "解除静音（播报结束 + 静音确认）")
            elif now - muted_since > 30:  # play_end 丢失保护
                muted, unmute_at = False, 0.0
                utt_start = last_voice = now
                log("asr", "静音超时强制解除（play_end 未到）")
        # 超时也落到终点判定：流停推后（mic 断/尾静音）最后一utterance必须能出终点
        chunks = []
        try:
            pull.setsockopt(zmq.RCVTIMEO, 50)
            chunks.append(np.frombuffer(pull.recv(), dtype=np.float32))
        except zmq.Again:
            pass
        try:
            while True:
                chunks.append(mic_q.get_nowait())
        except queue.Empty:
            pass
        for pcm in chunks:
            if muted:
                # 静音期只做能量观测（不喂引擎）：播报声持续推后解除时刻
                if float(np.sqrt(np.mean(pcm * pcm))) > vad_rms:
                    muted_voice_at = now
                continue
            engine.accept_pcm(pcm)
            engine.decode_available()
            # 能量 VAD 定终点：不能按 token 输出间隔判静音——整词单 token 的语言
            # （英文 BPE）正常语音间隔可超 1s，会造成句中假终点+冷启动碎片
            if float(np.sqrt(np.mean(pcm * pcm))) > vad_rms:
                last_voice = time.time()
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


def main():
    _load_conf()
    asr_dir = conf("voice.asr_dir", os.path.expanduser("~/rk_asr"))

    signal.signal(signal.SIGINT, lambda *_: globals().__setitem__("stop_flag", True))

    asr = AsrEngine(asr_dir)
    log("asr", "三模型 NPU 加载完成 (enc core0 / dec core1 / joi core2)")

    ctx = zmq.Context()
    asr_worker(ctx, asr)
    log("voice", "退出")


if __name__ == "__main__":
    main()
