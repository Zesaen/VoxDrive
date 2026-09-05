"""ctx — 插件共享服务中枢（PluginContext）。

数据源（core 内部线程，插件禁止自建 ZMQ/QThread）：
  ZmqHub         单线程 poller 订阅 4 路 SUB：6670 车控态 / 6671 全局状态
                 （含 asr_final/tts_say/nav，见设计规范 7.2）/ 6678 播报结束 / RK 事件
  DashcamPoller  REQ 轮询 RK :6700 结构化状态 → store.rk_status
  StreamPlayer   ffmpeg 拉 RTSP 出 BGRA 帧 → sig_frame

插件经 ctx 拿到的一切能力见 PluginContext 文档串。
"""

import json
import os
import subprocess
import sys
import time

import zmq
from PyQt5.QtCore import QObject, QThread, QTimer, QThreadPool, QRunnable, pyqtSignal

from common import vox_config


# ── 数据源线程 ─────────────────────────────────────────────────────────

class ZmqHub(QThread):
    """单线程 4 路 SUB poller → Qt 信号（原 dashboard_ui.ZmqSub 扩展语音通道）。"""

    sig_state = pyqtSignal(dict)       # tool_bus 6670 state_full/state_change
    sig_status = pyqtSignal(dict)      # 6671 全局状态/asr_final/tts_say/nav
    sig_play_end = pyqtSignal()        # 6678 裸串 play_end
    sig_rk_event = pyqtSignal(dict)    # RK 6671→6701 事件信封

    def __init__(self):
        super().__init__()
        self.ok = True

    def run(self):
        ctx = zmq.Context()
        socks = {}
        for ep, sig in [
            (vox_config.connect_endpoint("port.tool_bus_pub", "6670"), self.sig_state),
            (vox_config.connect_endpoint("port.intent_router_pub", "6671"), self.sig_status),
        ]:
            s = ctx.socket(zmq.SUB)
            s.connect(ep)
            s.setsockopt(zmq.SUBSCRIBE, b"")
            socks[s] = sig
        s_tts = ctx.socket(zmq.SUB)
        s_tts.connect(vox_config.connect_endpoint("port.tts_pub", "6678"))
        s_tts.setsockopt(zmq.SUBSCRIBE, b"")
        socks[s_tts] = self.sig_play_end
        s_rk = ctx.socket(zmq.SUB)
        s_rk.connect("tcp://%s:%s" % (vox_config.get("rk.ip", "192.168.137.200"),
                                      vox_config.get("rk.event_port", "6701")))
        s_rk.setsockopt(zmq.SUBSCRIBE, b"")
        socks[s_rk] = self.sig_rk_event
        poller = zmq.Poller()
        for s in socks:
            poller.register(s, zmq.POLLIN)
        while self.ok:
            for sock, _ in dict(poller.poll(300)).items():
                try:
                    msg = sock.recv_string(zmq.NOBLOCK)
                except zmq.Again:
                    continue
                sig = socks[sock]
                if sig is self.sig_play_end:            # TTS 裸串协议
                    if msg.strip() == "play_end":
                        sig.emit()
                    continue
                try:
                    sig.emit(json.loads(msg))
                except (json.JSONDecodeError, ValueError):
                    continue  # 脏消息跳过
        ctx.destroy()

    def stop(self):
        self.ok = False


class DashcamPoller(QThread):
    """周期轮询 RK recorder_service 状态（REQ :rk.status_port，消息信封）。"""

    sig_status = pyqtSignal(object)  # dict=状态 payload；None=RK 离线

    def __init__(self, interval_s=5.0):
        super().__init__()
        self.ok = True
        self.interval = interval_s
        self.wake = False

    def poke(self):
        self.wake = True

    def run(self):
        ep = "tcp://%s:%s" % (vox_config.get("rk.ip", "192.168.137.200"),
                              vox_config.get("rk.status_port", "6700"))
        ctx = zmq.Context()
        while self.ok:
            req = ctx.socket(zmq.REQ)
            req.setsockopt(zmq.RCVTIMEO, 2000)
            req.setsockopt(zmq.LINGER, 0)
            req.connect(ep)
            try:
                req.send_string(json.dumps({
                    "version": 1, "type": "status", "timestamp_ms": int(time.time() * 1000),
                    "source": "jetson.dashboard", "payload": {"cmd": "status"}}))
                env = json.loads(req.recv_string())
                self.sig_status.emit(env.get("payload"))
            except (zmq.Again, zmq.ZMQError, json.JSONDecodeError, ValueError):
                self.sig_status.emit(None)
            finally:
                req.close()
            deadline = time.time() + self.interval
            while self.ok and not self.wake and time.time() < deadline:
                self.msleep(100)
            self.wake = False
        ctx.destroy()

    def stop(self):
        self.ok = False


class StreamPlayer(QThread):
    """ffmpeg 拉 RTSP 出 BGRA 帧流 → QImage 信号（断流自动重连）。"""

    sig_frame = pyqtSignal(object)  # QImage
    sig_state = pyqtSignal(str)     # live / retry / off

    PV_WID = 960
    PV_HGT = 540

    def __init__(self, url):
        super().__init__()
        self.url = url
        self.ok = True
        self.enabled = False

    def set_enabled(self, on):
        self.enabled = on
        if not on:
            self.sig_state.emit("off")

    def run(self):
        frame_bytes = self.PV_WID * self.PV_HGT * 4
        cmd = ["ffmpeg", "-loglevel", "error",
               "-rtsp_transport", "tcp", "-fflags", "nobuffer", "-flags", "low_delay",
               "-i", self.url,
               "-vf", "scale=%d:%d" % (self.PV_WID, self.PV_HGT),
               "-f", "rawvideo", "-pix_fmt", "bgra", "-"]
        while self.ok:
            if not self.enabled:
                self.msleep(200)
                continue
            try:
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL)
            except OSError:
                # 无 ffmpeg 环境（PC 自测）：退化为占位循环，不崩
                self.sig_state.emit("retry")
                deadline = time.time() + 2
                while self.ok and self.enabled and time.time() < deadline:
                    self.msleep(100)
                continue
            self.sig_state.emit("live")
            while self.ok and self.enabled:
                buf = proc.stdout.read(frame_bytes)
                if not buf or len(buf) < frame_bytes:
                    break
                from PyQt5.QtGui import QImage
                img = QImage(buf, self.PV_WID, self.PV_HGT, QImage.Format_ARGB32)
                self.sig_frame.emit(img.copy())
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
            if self.ok and self.enabled:
                self.sig_state.emit("retry")
                deadline = time.time() + 2
                while self.ok and self.enabled and time.time() < deadline:
                    self.msleep(100)

    def stop(self):
        self.ok = False


# ── PluginContext ─────────────────────────────────────────────────────

class _Bus(QObject):
    """中央信号枢纽（ZmqHub 信号直通 + 语义别名）。"""
    state_changed = pyqtSignal(dict)      # 车控合并态
    service_status = pyqtSignal(dict)     # 6671 任意 JSON
    rk_event = pyqtSignal(dict)
    play_end = pyqtSignal()
    asr_text = pyqtSignal(str)
    tts_text = pyqtSignal(str)
    nav = pyqtSignal(str)
    rk_online = pyqtSignal(bool)
    frame = pyqtSignal(object)
    stream_state = pyqtSignal(str)
    dashcam_status = pyqtSignal(object)
    call_in_main = pyqtSignal(object)     # 工作线程 → 主线程内联回调（排队连接）

    def dispatch_in_main(self, fn):
        """run_async/queued 回调统一经此信号回主线程（跨线程碰 UI = 段错误）。"""
        try:
            fn()
        except Exception:  # noqa: BLE001 —— UI 回调异常不杀主循环
            import traceback
            traceback.print_exc()


class PluginContext:
    """插件与系统打交道的唯一通道（设计规范 5.3）。

    ctx.conf / ctx.theme / ctx.bus / ctx.store / ctx.tool / ctx.poll /
    ctx.run_async / ctx.attach_thread / ctx.toast / ctx.navigate
    """

    def __init__(self, theme_mgr, host):
        self.conf = vox_config
        self.theme = theme_mgr
        self.host = host  # MainWindow（提供 toast/navigate/dock 高亮）
        self.bus = _Bus()
        self.bus.call_in_main.connect(self.bus.dispatch_in_main)
        self.store = {}   # 共享状态仓：rk_status / sensor / ...
        self.player = None   # start_sources 装配（页面构建早于数据源，槽内须防 None）
        self.poller = None
        self._timers = {}
        self._threads = []
        self._pool = QThreadPool.globalInstance()

    # ── 工具调用（:6669 REQ，异步，2s 超时静默→on_done(None)）───────────
    def tool(self, tool_id, action, payload=None, on_done=None):
        req = {"tool": tool_id, "action": action}
        if payload:
            req.update(payload)
        self.run_async(lambda: _tool_req(req), on_done)

    # ── 主线程轮询（插件唯一合法定时器通道）────────────────────────────
    def poll(self, interval_ms, fn):
        t = QTimer(self.host)
        t.setInterval(interval_ms)
        t.timeout.connect(fn)
        t.start()
        self._timers[id(t)] = t
        return t

    # ── 一次性后台任务（on_done 经 call_in_main 信号回主线程）────────────
    def run_async(self, fn, on_done=None):
        bus = self.bus

        class _Task(QRunnable):
            def run(self_):
                try:
                    r = fn()
                except Exception:  # noqa: BLE001 —— 后台任务异常不进 UI 线程
                    r = None
                if on_done:
                    bus.call_in_main.emit(lambda r=r: on_done(r))
        self._pool.start(_Task())

    def attach_thread(self, qthread):
        """长任务线程注册（宿主 closeEvent 统一回收）。"""
        self._threads.append(qthread)
        return qthread

    # ── UI 动作（宿主实现）───────────────────────────────────────────
    def toast(self, text, level="info"):
        self.host.toast(text, level)

    def navigate(self, plugin_id):
        self.host.navigate(plugin_id)


def _tool_req(req):
    """同步 tool_bus REQ（在 QThreadPool 线程执行）。失败返回 None。"""
    try:
        ctx = zmq.Context()
        sk = ctx.socket(zmq.REQ)
        sk.setsockopt(zmq.RCVTIMEO, 2000)
        sk.setsockopt(zmq.LINGER, 0)
        sk.connect(vox_config.connect_endpoint("port.tool_bus", "6669"))
        sk.send_string(json.dumps(req))
        reply = sk.recv_string()
        sk.close()
        return json.loads(reply)
    except (zmq.ZMQError, zmq.Again, json.JSONDecodeError, ValueError):
        return None


def start_sources(ctx):
    """装配数据源线程并接到 bus（宿主启动时调用一次）。"""
    hub = ZmqHub()
    hub.sig_state.connect(ctx.bus.state_changed)
    hub.sig_status.connect(ctx.bus.service_status)
    hub.sig_play_end.connect(ctx.bus.play_end)
    hub.sig_rk_event.connect(ctx.bus.rk_event)
    ctx.attach_thread(hub)

    poller = DashcamPoller(vox_config.get_float("dashboard.dashcam_poll_s", 5.0))

    def _on_rk_status(p):
        was_online = ctx.store.get("rk_online", False)
        ctx.store["rk_status"] = p
        online = p is not None
        ctx.store["rk_online"] = online
        ctx.bus.dashcam_status.emit(p)
        if online != was_online:
            ctx.bus.rk_online.emit(online)

    poller.sig_status.connect(_on_rk_status)
    ctx.attach_thread(poller)

    player = StreamPlayer(vox_config.get("dashboard.preview_url",
                                         "rtsp://127.0.0.1:8554/live/dashcam"))
    player.sig_frame.connect(ctx.bus.frame)
    player.sig_state.connect(ctx.bus.stream_state)
    ctx.attach_thread(player)
    ctx.player = player
    ctx.poller = poller

    # 6671 → 语音语义信号分流（asr_final/tts_say/nav）
    def _on_service_status(m):
        ctx.bus.service_status.emit(m)
        s = m.get("service", "")
        st = m.get("status", "")
        if s == "router":
            if st == "asr_final" and m.get("asr_text"):
                ctx.bus.asr_text.emit(m["asr_text"])
            elif st == "tts_say" and m.get("tts_text"):
                ctx.bus.tts_text.emit(m["tts_text"])
            elif st == "nav" and m.get("target"):
                ctx.bus.nav.emit(m["target"])

    hub.sig_status.connect(_on_service_status)

    if os.environ.get("VOX_DASH_AUTOPREVIEW") == "1":
        player.set_enabled(True)
    hub.start()
    poller.start()
    player.start()
    return hub
