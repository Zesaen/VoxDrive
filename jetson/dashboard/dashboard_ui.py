#!/usr/bin/env python3
"""数字座舱 Dashboard（PyQt5）。

SUB :port.tool_bus_pub（车控状态）+ :port.intent_router_pub（服务状态）→ 界面刷新；
REQ :port.tool_bus 执行按钮命令。端口读 voxdrive.conf。

R8 扩展点：中部摄像头区（self.cam_view 文本占位）将替换为 RTMP 拉流渲染面板
（行车记录预览），右侧面板组追加"录像/存储状态"卡片；届时在此挂载，不动其他布局。
"""

import datetime
import json
import pathlib
import sys

JETSON_ROOT = pathlib.Path(__file__).resolve().parents[1]  # dashboard/ → jetson/
sys.path.insert(0, str(JETSON_ROOT))

import zmq  # noqa: E402
from PyQt5.QtCore import Qt, QThread, QTimer, pyqtSignal  # noqa: E402
from PyQt5.QtGui import QColor, QFont, QPainter, QPalette  # noqa: E402
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

VIEW_NAMES = {"front": "前视", "rear": "后视", "left": "左视", "right": "右视"}
MODE_MAP   = {"cool": "制冷", "heat": "制热", "vent": "通风"}
ROOF_MAP   = {"closed": "关闭", "open": "打开", "tilted": "翘起"}
DEFAULT_S  = {"ac_on": "false", "ac_temp": "24", "ac_mode": "vent", "ac_fan": "2",
              "window_fl": "0", "window_fr": "0", "window_rl": "0", "window_rr": "0",
              "sunroof_state": "closed", "seat_driver": "0", "seat_passenger": "0",
              "camera_view": "front", "camera_recording": "false"}
FONT       = "Noto Sans CJK SC"


class ZmqSub(QThread):
    """订阅 tool_bus 状态(6670) 与全局服务状态(6671)，转 Qt 信号。"""

    sig_state = pyqtSignal(dict)
    sig_status = pyqtSignal(dict)

    def __init__(self):
        super().__init__()
        self.ok = True

    def run(self):
        ctx = zmq.Context()
        s_state = ctx.socket(zmq.SUB)
        s_state.connect(vox_config.connect_endpoint("port.tool_bus_pub", "6670"))
        s_state.setsockopt(zmq.SUBSCRIBE, b"")
        s_status = ctx.socket(zmq.SUB)
        s_status.connect(vox_config.connect_endpoint("port.intent_router_pub", "6671"))
        s_status.setsockopt(zmq.SUBSCRIBE, b"")
        poller = zmq.Poller()
        poller.register(s_state, zmq.POLLIN)
        poller.register(s_status, zmq.POLLIN)
        while self.ok:
            for sock, _ in dict(poller.poll(300)).items():
                try:
                    msg = json.loads(sock.recv_string(zmq.NOBLOCK))
                except (zmq.Again, json.JSONDecodeError, ValueError):
                    continue  # NOBLOCK 竞态/脏消息：跳过，下一轮继续
                (self.sig_state if sock == s_state else self.sig_status).emit(msg)
        ctx.destroy()

    def stop(self):
        self.ok = False


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

        # ── 主区：摄像头 65% | 面板 35% ──
        mr = QHBoxLayout()
        mr.setSpacing(10)

        # 摄像头区（R8 扩展点：cam_view 文本占位 → 拉流渲染面板）
        cf = QFrame()
        cf.setObjectName("card")
        cf.setStyleSheet(CARD)
        cl = QVBoxLayout(cf)
        cl.setContentsMargins(0, 0, 0, 0)
        self.cam_view = QLabel("前 视")
        self.cam_view.setAlignment(Qt.AlignCenter)
        self.cam_view.setFont(QFont(FONT, 48, QFont.Bold))
        self.cam_view.setStyleSheet(
            f"color:rgba(0,200,255,0.15); background:#000; border-radius:14px; border:none;")
        self.cam_view.setMinimumSize(640, 380)
        cl.addWidget(self.cam_view)
        cb = QHBoxLayout()
        cb.setContentsMargins(12, 8, 12, 12)
        self.cam_btns = {}
        for k, n in VIEW_NAMES.items():
            b = QPushButton(n)
            b.setStyleSheet(BTN)
            b.clicked.connect(lambda _, kk=k: self._cmd("camera_capture", kk))
            cb.addWidget(b)
            self.cam_btns[k] = b
        self.rec_btn = QPushButton("● REC")
        self.rec_btn.setStyleSheet(BTN)
        self.rec_btn.clicked.connect(self._toggle_rec)
        cb.addWidget(self.rec_btn)
        cb.addStretch()
        cl.addLayout(cb)
        mr.addWidget(cf, 65)

        # 右侧面板组（R8 扩展点：追加"录像/存储状态"卡片）
        rp = QVBoxLayout()
        rp.setSpacing(8)

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
        self.z.start()
        self._cmd_ctx = zmq.Context()

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
        v = s.get("camera_view", "front")
        self.cam_view.setText(VIEW_NAMES.get(v, v))
        rec = s.get("camera_recording") in ("true", True)
        self.rec_btn.setText("● REC" if rec else "○ REC")
        self.rec_btn.setStyleSheet(BTN_REC_ON if rec else BTN)

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

    def _toggle_rec(self):
        r = self.state.get("camera_recording") in ("true", True)
        self._cmd("camera_capture", "record_off" if r else "record_on")

    def closeEvent(self, e):
        self.z.stop()
        self.z.wait(1000)
        e.accept()


def main():
    vox_config.load()
    a = QApplication(sys.argv)
    a.setStyle("Fusion")
    p = QPalette()
    p.setColor(QPalette.Window, QColor(BG))
    p.setColor(QPalette.WindowText, QColor(T1))
    a.setPalette(p)
    Dashboard().show()
    sys.exit(a.exec_())


if __name__ == "__main__":
    main()
