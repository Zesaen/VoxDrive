#!/usr/bin/env python3
"""数字座舱 Dashboard（PyQt5）。

SUB :port.tool_bus_pub（车控状态）+ :port.intent_router_pub（服务状态）
   + tcp://rk.ip:rk.event_port（RK 行车记录异常事件 → 告警横幅）→ 界面刷新；
REQ :port.tool_bus 执行按钮命令；REQ rk.ip:rk.status_port 轮询行车记录状态；
ffmpeg 子进程拉 mediamtx RTSP 出 BGRA 帧流 → 摄像头区实时预览（D3/R8）。
端口读 voxdrive.conf。

自测钩子（无屏验证）：VOX_DASH_SNAPSHOT=/tmp/x.png [VOX_DASH_SNAPSHOT_S=12]
[VOX_DASH_AUTOPREVIEW=1] —— 延时 N 秒截图整窗后自动退出。
"""

import datetime
import json
import os
import pathlib
import subprocess
import sys
import time

JETSON_ROOT = pathlib.Path(__file__).resolve().parents[1]  # dashboard/ → jetson/
sys.path.insert(0, str(JETSON_ROOT))

import zmq  # noqa: E402
from PyQt5.QtCore import Qt, QThread, QTimer, pyqtSignal  # noqa: E402
from PyQt5.QtGui import QColor, QFont, QImage, QPainter, QPalette, QPixmap  # noqa: E402
from PyQt5.QtWidgets import (QApplication, QFrame, QHBoxLayout, QLabel, QMainWindow,  # noqa: E402
                             QPushButton, QProgressBar, QVBoxLayout, QWidget)

from common import vox_config  # noqa: E402

# ── Design System ──
BG       = "#080c12"
CARD_BG  = "#131821"
ACCENT   = "#00c8ff"
ACCENT2  = "#00e676"
WARN     = "#ff9100"
DANGER   = "#ff1744"
T1       = "#eceff1"
T2       = "#78909c"
BORDER   = "#1a2332"
REC_BG   = "#0d2a1a"   # 录像中 REC 按钮底色

CARD = f"""
    QFrame#card {{
        background: {CARD_BG};
        border: 1px solid {BORDER};
        border-radius: 14px;
    }}
"""

BTN = f"""
    QPushButton {{
        background: {CARD_BG};
        color: {T1};
        border: 1px solid {BORDER};
        border-radius: 10px;
        padding: 10px 20px;
        font-size: 15px;
        font-weight: 500;
    }}
    QPushButton:hover {{
        background: #1a2a3a;
        border-color: {ACCENT};
    }}
"""

BTN_REC_ON = f"""
    QPushButton {{
        background: {REC_BG};
        color: {T1};
        border: 1px solid {ACCENT2};
        border-radius: 10px;
        padding: 10px 20px;
        font-size: 15px;
        font-weight: 500;
    }}
"""

PROG = f"""
    QProgressBar {{
        background: #0d1117;
        border: none;
        border-radius: 3px;
        height: 10px;
    }}
    QProgressBar::chunk {{
        background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 {ACCENT},stop:1 {ACCENT2});
        border-radius: 3px;
    }}
"""

MODE_MAP   = {"cool": "制冷", "heat": "制热", "vent": "通风"}
ROOF_MAP   = {"closed": "关闭", "open": "打开", "tilted": "翘起"}
DEFAULT_S  = {"ac_on": "false", "ac_temp": "24", "ac_mode": "vent", "ac_fan": "2",
              "window_fl": "0", "window_fr": "0", "window_rl": "0", "window_rr": "0",
              "sunroof_state": "closed", "seat_driver": "0", "seat_passenger": "0"}
FONT       = "Noto Sans CJK SC"

# RK 行车记录异常事件 → 告警横幅（背景色, 边框色, 话术）；segment_* 等信息类事件不弹横幅
ALERTS = {
    "watermark_deleted":   ("rgba(255,145,0,0.16)",  WARN,   "存储超水位：已自动删除最旧录像段"),
    "write_error":         ("rgba(255,23,68,0.16)",  DANGER, "录像写入异常，正在恢复"),
    "capture_timeout":     ("rgba(255,23,68,0.16)",  DANGER, "摄像头采集超时，请检查行车记录仪"),
    "rtmp_disconnected":   ("rgba(255,145,0,0.16)",  WARN,   "预览推流中断，自动重连中"),
    "rtmp_connect_failed": ("rgba(255,145,0,0.16)",  WARN,   "预览推流连接失败，稍后自动重试"),
}


class ZmqSub(QThread):
    """订阅 tool_bus 状态(6670)、全局服务状态(6671)、RK 行车记录事件(rk.event_port)，转 Qt 信号。"""

    sig_state = pyqtSignal(dict)
    sig_status = pyqtSignal(dict)
    sig_rk_event = pyqtSignal(dict)

    def __init__(self):
        super().__init__()
        self.ok = True

    def run(self):
        ctx = zmq.Context()
        socks = {}
        s_state = ctx.socket(zmq.SUB)
        s_state.connect(vox_config.connect_endpoint("port.tool_bus_pub", "6670"))
        s_state.setsockopt(zmq.SUBSCRIBE, b"")
        socks[s_state] = self.sig_state
        s_status = ctx.socket(zmq.SUB)
        s_status.connect(vox_config.connect_endpoint("port.intent_router_pub", "6671"))
        s_status.setsockopt(zmq.SUBSCRIBE, b"")
        socks[s_status] = self.sig_status
        s_rk = ctx.socket(zmq.SUB)
        s_rk.connect("tcp://%s:%s" % (
            vox_config.get("rk.ip", "192.168.137.200"),
            vox_config.get("rk.event_port", "6701")))
        s_rk.setsockopt(zmq.SUBSCRIBE, b"")
        socks[s_rk] = self.sig_rk_event
        poller = zmq.Poller()
        for s in socks:
            poller.register(s, zmq.POLLIN)
        while self.ok:
            for sock, _ in dict(poller.poll(300)).items():
                try:
                    msg = json.loads(sock.recv_string(zmq.NOBLOCK))
                except (zmq.Again, json.JSONDecodeError, ValueError):
                    continue  # NOBLOCK 竞态/脏消息：跳过，下一轮继续
                socks[sock].emit(msg)
        ctx.destroy()

    def stop(self):
        self.ok = False


class DashcamPoller(QThread):
    """周期轮询 RK recorder_service 状态（REQ :rk.status_port，消息信封），结构化数据直读。

    与 DashcamControl 工具同协议但直达 RK 状态口：UI 需要结构化字段（帧率/水位/段数），
    走 tool_bus 只能拿到组织好的中文字符串。read-only，不越过工具总线做控制。
    """

    sig_status = pyqtSignal(object)  # dict=状态 payload；None=RK 离线

    def __init__(self, interval_s=5.0):
        super().__init__()
        self.ok = True
        self.interval = interval_s
        self.wake = False  # poke() 置位后立即轮询一轮（命令后回读）

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
            # poke 或到点再轮询（Event 语义用循环等待实现，避免额外锁）
            deadline = time.time() + self.interval
            while self.ok and not self.wake and time.time() < deadline:
                self.msleep(100)
            self.wake = False
        ctx.destroy()

    def stop(self):
        self.ok = False


class StreamPlayer(QThread):
    """ffmpeg 拉 RTSP 出 BGRA 帧流 → QImage 信号（断流自动重连，预览开关即时生效）。

    面板渲染路径：ffmpeg 解码 + 缩放到 PV_WID 宽（降低 Orin 上 UI 拷贝量），
    rawvideo 管道读整帧；QImage 必须 .copy() 脱离管道缓冲后才跨线程安全。
    """

    sig_frame = pyqtSignal(QImage)
    sig_state = pyqtSignal(str)  # live / retry / off

    PV_WID = 960
    PV_HGT = 540  # 16:9，源 1080p 等比缩放

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
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL)
            self.sig_state.emit("live")
            while self.ok and self.enabled:
                buf = proc.stdout.read(frame_bytes)
                if not buf or len(buf) < frame_bytes:
                    break  # 流结束/ffmpeg 退出（RK 停推或 mediamtx 重启）
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


class BarGauge(QFrame):
    """水平条形仪表：彩色进度条。"""

    def __init__(self, val=0, maxv=100, color=ACCENT):
        super().__init__()
        self.setStyleSheet("background:transparent; border:none;")
        self.bar_color = QColor(color)
        self.val = val
        self.maxv = maxv
        self.setMinimumHeight(24)
        self.setMaximumHeight(28)

    def setVal(self, v):
        self.val = v
        self.update()

    def paintEvent(self, e):
        qp = QPainter(self)
        qp.setRenderHint(QPainter.Antialiasing)
        w = self.width()
        h = self.height()
        qp.setPen(Qt.NoPen)
        qp.setBrush(QColor("#0d1117"))
        qp.drawRoundedRect(0, h - 10, w, 10, 5, 5)
        pct = min(self.val / self.maxv, 1.0)
        if pct > 0:
            qp.setBrush(self.bar_color)
            qp.drawRoundedRect(0, h - 10, int(w * pct), 10, 5, 5)


class Dashboard(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("数字座舱 · Vehicle AI")
        self.setStyleSheet(f"background:{BG};")
        self.state = dict(DEFAULT_S)
        self.dash = {"online": False}  # RK 行车记录状态缓存（poller 维护）
        self._ui()
        self._zmq()
        self.clock = QTimer()
        self.clock.timeout.connect(self._tick)
        self.clock.start(1000)
        self._tick()

    def _ui(self):
        cw = QWidget()
        self.setCentralWidget(cw)
        root = QVBoxLayout(cw)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(10)

        # ── 状态条 ──
        sf = QFrame()
        sf.setObjectName("card")
        sf.setStyleSheet(CARD)
        sf.setFixedHeight(36)
        sl = QHBoxLayout(sf)
        sl.setContentsMargins(16, 0, 16, 0)
        self.slbl = {}
        for n in ["router", "rag", "llm", "tts", "tool"]:
            lb = QLabel("· " + n)
            lb.setFont(QFont(FONT, 10))
            lb.setStyleSheet(f"color:#445; background:transparent; border:none;")
            sl.addWidget(lb)
            self.slbl[n] = lb
        sl.addStretch()
        self.clk = QLabel()
        self.clk.setFont(QFont(FONT, 13, QFont.Bold))
        self.clk.setStyleSheet(f"color:{T1}; background:transparent; border:none;")
        sl.addWidget(self.clk)
        root.addWidget(sf)

        # ── RK 异常事件告警横幅（默认隐藏，事件驱动显示，8s 后自动收回）──
        self.alert = QLabel()
        self.alert.setFont(QFont(FONT, 12, QFont.Bold))
        self.alert.setStyleSheet("background:transparent; border:none; padding:6px 14px;")
        self.alert.hide()
        root.addWidget(self.alert)
        self.alert_timer = QTimer()
        self.alert_timer.setSingleShot(True)
        self.alert_timer.timeout.connect(self.alert.hide)

        # ── 主区：摄像头 65% | 面板 35% ──
        mr = QHBoxLayout()
        mr.setSpacing(10)

        # 行车记录预览区（D3/R8：ffmpeg 拉流渲染，未开时文本占位）
        cf = QFrame()
        cf.setObjectName("card")
        cf.setStyleSheet(CARD)
        cl = QVBoxLayout(cf)
        cl.setContentsMargins(0, 0, 0, 0)
        ch = QHBoxLayout()
        ch.setContentsMargins(14, 10, 14, 0)
        ch.addWidget(self._hl("🚗 行车记录 · 实时预览", 13, ACCENT))
        ch.addStretch()
        self.live_pill = QLabel("○ 预览关")
        self.live_pill.setFont(QFont(FONT, 11, QFont.Bold))
        self.live_pill.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
        ch.addWidget(self.live_pill)
        cl.addLayout(ch)
        self.cam_view = QLabel("预览未开启")
        self.cam_view.setAlignment(Qt.AlignCenter)
        self.cam_view.setFont(QFont(FONT, 36, QFont.Bold))
        self.cam_view.setStyleSheet(
            f"color:rgba(0,200,255,0.15); background:#000; border-radius:14px; border:none;")
        self.cam_view.setMinimumSize(640, 380)
        cl.addWidget(self.cam_view)
        cb = QHBoxLayout()
        cb.setContentsMargins(12, 8, 12, 12)
        # 真设备按钮组：录像开关 / 预览开关 / 跨板抓拍（与语音同走 tool_bus → RK）
        self.rec_btn = QPushButton("● REC")
        self.rec_btn.setStyleSheet(BTN)
        self.rec_btn.clicked.connect(self._toggle_rec)
        cb.addWidget(self.rec_btn)
        self.pv_btn = QPushButton("预览 开")
        self.pv_btn.setStyleSheet(BTN)
        self.pv_btn.clicked.connect(self._toggle_preview)
        cb.addWidget(self.pv_btn)
        self.snap_btn = QPushButton("📸 抓拍")
        self.snap_btn.setStyleSheet(BTN)
        self.snap_btn.clicked.connect(self._snap)
        cb.addWidget(self.snap_btn)
        cb.addStretch()
        cl.addLayout(cb)
        mr.addWidget(cf, 65)

        # 右侧面板组
        rp = QVBoxLayout()
        rp.setSpacing(8)

        # 行车记录状态卡片（D3/R8：poller 5s 回读 RK 结构化状态）
        dc = QFrame()
        dc.setObjectName("card")
        dc.setStyleSheet(CARD)
        dcl = QVBoxLayout(dc)
        dcl.setSpacing(6)
        dh = QHBoxLayout()
        dh.addWidget(self._hl("🚗 行车记录仪", 13, ACCENT))
        dh.addStretch()
        self.dc_state = QLabel("连接中…")
        self.dc_state.setFont(QFont(FONT, 14, QFont.Bold))
        self.dc_state.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
        dh.addWidget(self.dc_state)
        dcl.addLayout(dh)
        drow = QHBoxLayout()
        self.dc_fps = QLabel("-- fps")
        self.dc_fps.setFont(QFont(FONT, 12))
        self.dc_fps.setStyleSheet(f"color:{ACCENT2}; background:transparent; border:none;")
        drow.addWidget(self.dc_fps)
        drow.addStretch()
        self.dc_segs = QLabel("-- 段")
        self.dc_segs.setFont(QFont(FONT, 12))
        self.dc_segs.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
        drow.addWidget(self.dc_segs)
        self.dc_free = QLabel("-- GB 可用")
        self.dc_free.setFont(QFont(FONT, 12))
        self.dc_free.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
        drow.addWidget(self.dc_free)
        dcl.addLayout(drow)
        self.dc_bar = BarGauge()
        dcl.addWidget(self.dc_bar)
        rp.addWidget(dc)

        # 空调卡片
        ac = QFrame()
        ac.setObjectName("card")
        ac.setStyleSheet(CARD)
        acl = QVBoxLayout(ac)
        acl.setSpacing(6)
        ah = QHBoxLayout()
        ah.addWidget(self._hl("❄ 空调系统", 13, ACCENT))
        ah.addStretch()
        self.ac_on = QLabel("OFF")
        self.ac_on.setFont(QFont(FONT, 14, QFont.Bold))
        self.ac_on.setStyleSheet(f"color:{DANGER}; background:transparent; border:none;")
        ah.addWidget(self.ac_on)
        acl.addLayout(ah)
        self.ac_temp = QLabel("24°C")
        self.ac_temp.setFont(QFont(FONT, 42, QFont.Bold))
        self.ac_temp.setStyleSheet(f"color:{T1}; background:transparent; border:none;")
        acl.addWidget(self.ac_temp)
        ad = QHBoxLayout()
        self.ac_mode = QLabel("通风")
        self.ac_mode.setFont(QFont(FONT, 13))
        self.ac_mode.setStyleSheet(f"color:{ACCENT2}; background:transparent; border:none;")
        ad.addWidget(self.ac_mode)
        ad.addStretch()
        self.ac_fan = QLabel("风量 2")
        self.ac_fan.setFont(QFont(FONT, 13))
        self.ac_fan.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
        ad.addWidget(self.ac_fan)
        acl.addLayout(ad)
        rp.addWidget(ac)

        # 车窗卡片
        wn = QFrame()
        wn.setObjectName("card")
        wn.setStyleSheet(CARD)
        wl = QVBoxLayout(wn)
        wl.setSpacing(4)
        wl.addWidget(self._hl("🪟 车窗", 13, ACCENT))
        self._wbar = {}
        for k, nm in [("window_fl", "左前"), ("window_fr", "右前"),
                      ("window_rl", "左后"), ("window_rr", "右后")]:
            row = QHBoxLayout()
            lb = QLabel(nm)
            lb.setFont(QFont(FONT, 12))
            lb.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
            lb.setFixedWidth(40)
            row.addWidget(lb)
            pb = QProgressBar()
            pb.setRange(0, 100)
            pb.setValue(0)
            pb.setStyleSheet(PROG)
            pb.setFixedHeight(8)
            row.addWidget(pb)
            wl.addLayout(row)
            self._wbar[k] = pb
        rp.addWidget(wn)

        # 天窗 + 座椅行
        ss = QHBoxLayout()
        ss.setSpacing(8)
        for title, nm in [("🌤 天窗", "roof"), ("🔥 座椅加热", "seat")]:
            f = QFrame()
            f.setObjectName("card")
            f.setStyleSheet(CARD)
            fl = QVBoxLayout(f)
            fl.setSpacing(4)
            fl.addWidget(self._hl(title, 12))
            vl = QLabel()
            vl.setFont(QFont(FONT, 14, QFont.Bold))
            vl.setStyleSheet(f"color:{T1}; background:transparent; border:none;")
            fl.addWidget(vl)
            ss.addWidget(f)
            setattr(self, f"{nm}_lbl", vl)
        rp.addLayout(ss)

        # 传感器卡片
        sn = QFrame()
        sn.setObjectName("card")
        sn.setStyleSheet(CARD)
        snl = QVBoxLayout(sn)
        snl.setSpacing(4)
        snl.addWidget(self._hl("📊 传感器数据", 13, ACCENT))
        self.sens = QLabel("等待数据...")
        self.sens.setFont(QFont(FONT, 12))
        self.sens.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
        self.sens.setWordWrap(True)
        snl.addWidget(self.sens)
        rp.addWidget(sn)

        mr.addLayout(rp, 35)
        root.addLayout(mr, 1)
        self.resize(1400, 780)

    def _hl(self, text, size=13, color=T1):
        w = QLabel(text)
        w.setFont(QFont(FONT, size, QFont.Bold))
        w.setStyleSheet(f"color:{color}; background:transparent; border:none;")
        return w

    def _zmq(self):
        self.z = ZmqSub()
        self.z.sig_state.connect(self._ons)
        self.z.sig_status.connect(self._onx)
        self.z.sig_rk_event.connect(self._on_rk_event)
        self.z.start()
        self._cmd_ctx = zmq.Context()
        self.poller = DashcamPoller(vox_config.get_float("dashboard.dashcam_poll_s", 5.0))
        self.poller.sig_status.connect(self._on_dashcam)
        self.poller.start()
        self.player = StreamPlayer(vox_config.get("dashboard.preview_url",
                                                  "rtsp://127.0.0.1:8554/live/dashcam"))
        self.player.sig_frame.connect(self._on_frame)
        self.player.sig_state.connect(self._on_stream_state)
        self.player.start()
        if os.environ.get("VOX_DASH_AUTOPREVIEW") == "1":
            self._toggle_preview(force_on=True)

    # ── 行车记录：状态轮询 / 预览渲染 / 按钮命令 ──

    def _on_dashcam(self, p):
        if p is None:
            self.dash = {"online": False}
            self.dc_state.setText("离线")
            self.dc_state.setStyleSheet(f"color:{DANGER}; background:transparent; border:none;")
            return
        self.dash = dict(p, online=True)
        rec = bool(p.get("recording"))
        self.dc_state.setText("● 录像中" if rec else "○ 暂停")
        self.dc_state.setStyleSheet(
            f"color:{ACCENT2 if rec else WARN}; background:transparent; border:none;")
        self.rec_btn.setText("● REC" if rec else "○ REC")
        self.rec_btn.setStyleSheet(BTN_REC_ON if rec else BTN)
        st = p.get("storage", {})
        self.dc_fps.setText("%.1f fps" % p.get("pipeline_fps", 0.0))
        self.dc_segs.setText("%d 段" % st.get("segments_total", 0))
        self.dc_free.setText("%.1f GB 可用" % st.get("free_gb", 0.0))
        self.dc_bar.setVal(st.get("used_percent", 0.0))

    def _on_frame(self, img):
        self.cam_view.setPixmap(QPixmap.fromImage(img))

    def _on_stream_state(self, s):
        if s == "live":
            self.live_pill.setText("● LIVE")
            self.live_pill.setStyleSheet(f"color:{ACCENT2}; background:transparent; border:none;")
        elif s == "retry":
            self.live_pill.setText("● 重连中")
            self.live_pill.setStyleSheet(f"color:{WARN}; background:transparent; border:none;")
            self.cam_view.setText("信号中断，重连中…")
        else:  # off
            self.live_pill.setText("○ 预览关")
            self.live_pill.setStyleSheet(f"color:{T2}; background:transparent; border:none;")
            self.cam_view.setText("预览未开启")

    def _toggle_preview(self, force_on=False):
        on = force_on or not self.player.enabled
        self.player.set_enabled(on)
        self.pv_btn.setText("预览 开" if on else "预览 关")
        # 同步 RK 推流开关（经 tool_bus，与语音同路）；离线/超时静默，拉流端自会重连
        self._cmd("dashcam", "preview_on" if on else "preview_off")

    def _snap(self):
        self._cmd("dashcam", "snapshot")
        print("[dashcam] snapshot requested -> %s" %
              vox_config.get("snapshot_dir", "$HOME/voxdrive_snapshots"), flush=True)

    def _toggle_rec(self):
        # 与语音同路：tool_bus dashcam 工具；命令后 poke 轮询器立即回读真实状态
        on = self.dash.get("online") and not self.dash.get("recording")
        self._cmd("dashcam", "record_on" if on else "record_off")
        self.poller.poke()

    def _tick(self):
        self.clk.setText(datetime.datetime.now().strftime("%H:%M"))

    def _ons(self, m):
        if m.get("type", "") in ("state_full", "state_change"):
            self.state.update(m.get("values", {}))
            self._rf()

    def _onx(self, m):
        s = m.get("service", "?")
        t = m.get("status", "")
        if s in self.slbl:
            self.slbl[s].setText(f"● {s}: {t}")
            self.slbl[s].setStyleSheet(
                f"color:{ACCENT2}; background:transparent; border:none; font-size:10px;")
            QTimer.singleShot(4000, lambda sv=s: (
                self.slbl[sv].setText(f"· {sv}"),
                self.slbl[sv].setStyleSheet(
                    "color:#445; background:transparent; border:none; font-size:10px;")))

    def _on_rk_event(self, m):
        """RK 行车记录事件（消息信封 type=event）：异常事件弹横幅，全量打 stdout 日志。

        stdout 带 [rk-event] 前缀供无屏自测与 R9 毫秒日志对账。
        """
        p = m.get("payload", {}) if isinstance(m, dict) else {}
        if not isinstance(p, dict):
            return
        ev = p.get("event", "")
        if not ev:
            return
        ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"[rk-event] {ts} {ev} "
              f"{json.dumps(p.get('detail', {}), ensure_ascii=False)}", flush=True)
        spec = ALERTS.get(ev)
        if not spec:
            return
        bg, border, text = spec
        self.alert.setText(f"⚠ {text}（{ts}）")
        self.alert.setStyleSheet(
            f"color:{T1}; background:{bg}; border:1px solid {border}; "
            f"border-radius:8px; padding:6px 14px;")
        self.alert.show()
        self.alert_timer.start(8000)

    def _rf(self):
        s = self.state
        on = s.get("ac_on") in ("true", True)
        self.ac_on.setText("● ON" if on else "OFF")
        self.ac_on.setStyleSheet(
            f"color:{ACCENT2 if on else DANGER}; background:transparent; border:none; font-size:14px;")
        self.ac_temp.setText(f"{s.get('ac_temp', '24')}°C")
        self.ac_mode.setText(MODE_MAP.get(s.get("ac_mode", "vent"), "通风"))
        self.ac_fan.setText(f"风量 {s.get('ac_fan', '2')}")
        for k in self._wbar:
            self._wbar[k].setValue(int(s.get(k, 0)))
        self.roof_lbl.setText(ROOF_MAP.get(s.get("sunroof_state", "closed"), "关闭"))
        d = int(s.get("seat_driver", 0))
        p = int(s.get("seat_passenger", 0))
        self.seat_lbl.setText(f"主{'●' + str(d) if d else '○'} | 副{'●' + str(p) if p else '○'}")

    def _cmd(self, tool, action, **kw):
        """按钮命令 → tool_bus（2s 超时，失败静默——UI 不因后端缺席卡死）。"""
        try:
            sk = self._cmd_ctx.socket(zmq.REQ)
            sk.setsockopt(zmq.RCVTIMEO, 2000)
            sk.setsockopt(zmq.LINGER, 0)
            sk.connect(vox_config.connect_endpoint("port.tool_bus", "6669"))
            sk.send_string(json.dumps({"tool": tool, "action": action, **kw}))
            sk.recv_string()
            sk.close()
        except zmq.ZMQError:
            pass

    def closeEvent(self, e):
        self.z.stop()
        self.z.wait(1000)
        self.player.stop()
        self.player.wait(3000)
        self.poller.stop()
        self.poller.wait(2000)
        e.accept()


def main():
    vox_config.load()
    a = QApplication(sys.argv)
    a.setStyle("Fusion")
    p = QPalette()
    p.setColor(QPalette.Window, QColor(BG))
    p.setColor(QPalette.WindowText, QColor(T1))
    a.setPalette(p)
    win = Dashboard()
    win.show()

    # 无屏自测钩子：延时截图整窗（含预览帧/状态卡片）后自动退出
    snap = os.environ.get("VOX_DASH_SNAPSHOT")
    if snap:
        def _grab():
            win.grab().save(snap)
            print(f"[dash-snapshot] saved {snap}", flush=True)
            a.quit()
        QTimer.singleShot(int(os.environ.get("VOX_DASH_SNAPSHOT_S", "12")) * 1000, _grab)

    sys.exit(a.exec_())


if __name__ == "__main__":
    main()
