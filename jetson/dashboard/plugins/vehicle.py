"""vehicle 插件 — 车控页（空调卡 + 车身俯视图：车窗×4/天窗/座椅）。

七工具中 4 个工具的控件在此（climate/window/sunroof/seat；camera_capture 归 dashcam 域）。
交互：乐观更新（立即反馈）→ ctx.tool → :6670 state_change/state_full 广播回读校正；
tool_bus 无应答 5s 后 toast 兜底（不阻塞、不卡死）。
"""

from PyQt5.QtCore import QRectF, Qt, QTimer
from PyQt5.QtGui import QColor, QPainter, QPainterPath, QPen
from PyQt5.QtWidgets import QHBoxLayout, QLabel, QPushButton, QVBoxLayout, QWidget

from core import icons  # noqa: F401 —— 主题切换时 IconLabel 自动重染
from core.widgets import Card, font, hline, set_pill

MANIFEST = {
    "id": "vehicle", "name": "车控", "icon": "car",
    "order": 20, "tile_span": 1, "page": True,
    "nav_words": ["车控", "车辆控制", "空调页面", "车控页面"],
}

MODES = [("cool", "制冷"), ("heat", "制热"), ("vent", "通风"), ("defrost", "除雾")]
ROOF_LABEL = {"closed": "关闭", "open": "打开", "tilted": "翘起"}

# 模块级车控状态镜像（:6670 广播维护；插件禁止自建线程，此 dict 只在主线程读写）
_STATE = {}
_pending = {}   # 乐观键 → 旧值（回读后清除；5s 未清=无应答）


def create_card(ctx):
    def _refresh():
        tile = ctx.host.tile_of("vehicle")
        if tile is not None:
            on = _STATE.get("ac_on") in ("true", True)
            tile.summary.setText(
                f"{'空调开' if on else '空调关'} · {_STATE.get('ac_temp', '24')}°C · "
                f"车窗{_win_sum()}")

    def _on_state(m):
        _STATE.update(m.get("values", {}))
        _refresh()

    ctx.bus.state_changed.connect(_on_state)
    _refresh()
    return None


def _win_sum():
    n = sum(1 for k in ("window_fl", "window_fr", "window_rl", "window_rr")
            if int(_STATE.get(k, 0) or 0) > 0)
    return f"{n}/4开"


def create_page(ctx):
    page = QWidget()
    root = QHBoxLayout(page)
    root.setContentsMargins(0, 0, 0, 0)
    root.setSpacing(16)

    # ── 左：空调卡 ──
    ac = Card()
    root.addWidget(ac, 46)
    al = ac.inner
    ac_on_lbl = QLabel("OFF")
    ac_on_lbl.setProperty("pill", True)
    al.addWidget(hline("空调", "thermometer", ac_on_lbl)[0])

    temp_lbl = QLabel("24°C")
    temp_lbl.setProperty("mono", True)
    temp_lbl.setStyleSheet("font-size: 42px; font-weight: 700;")
    temp_lbl.setAlignment(Qt.AlignCenter)
    al.addWidget(temp_lbl)

    adj = QHBoxLayout()
    adj.setSpacing(12)
    b_down = QPushButton("－")
    b_down.setMinimumHeight(48)
    b_down.setMinimumWidth(64)
    b_up = QPushButton("＋")
    b_up.setMinimumHeight(48)
    b_up.setMinimumWidth(64)
    b_power = QPushButton("电源")
    b_power.setMinimumHeight(48)
    adj.addWidget(b_down)
    adj.addWidget(b_up)
    adj.addStretch()
    adj.addWidget(b_power)
    al.addLayout(adj)

    b_down.clicked.connect(lambda: _ctl_temp(ctx, -1))
    b_up.clicked.connect(lambda: _ctl_temp(ctx, +1))
    b_power.clicked.connect(lambda: _ctl(ctx, "off" if _STATE.get("ac_on") in ("true", True)
                                         else "on"))

    mode_row = QHBoxLayout()
    mode_row.setSpacing(8)
    mode_btns = {}
    for mid, mname in MODES:
        b = QPushButton(mname)
        b.setMinimumHeight(44)
        b.setCheckable(True)
        b.clicked.connect(lambda _, m=mid: _ctl(ctx, "set_mode", {"mode": m}))
        mode_row.addWidget(b)
        mode_btns[mid] = b
    al.addLayout(mode_row)

    fan_row = QHBoxLayout()
    fan_row.setSpacing(8)
    fan_tip = QLabel("风量")
    fan_tip.setObjectName("t_muted")
    fan_row.addWidget(fan_tip)
    fan_btns = {}
    for lv in (1, 2, 3):
        b = QPushButton(str(lv))
        b.setMinimumHeight(44)
        b.setMinimumWidth(52)
        b.setCheckable(True)
        b.clicked.connect(lambda _, l=lv: _ctl(ctx, "set_fan", {"level": l}))
        fan_row.addWidget(b)
        fan_btns[lv] = b
    fan_row.addStretch()
    al.addLayout(fan_row)
    al.addStretch()  # 行高自然分布（无此行首行会被纵向拉伸、pill 变形）

    # ── 右：车身俯视图 ──
    cm_card = Card()
    root.addWidget(cm_card, 54)
    cml = cm_card.inner
    cml.addWidget(hline("车身状态（点击控制）", "car")[0])
    carmap = CarMap(ctx)
    cml.addWidget(carmap, 1)

    # ── :6670 回读 → UI 校正 ────────────────────────────────────────
    def _on_state(m):
        _STATE.update(m.get("values", {}))
        # 回读到达即清对应乐观挂起
        for k in m.get("values", {}):
            _pending.pop(k, None)
        if m.get("type") == "state_full":
            _pending.clear()
        on = _STATE.get("ac_on") in ("true", True)
        set_pill(ac_on_lbl, "● ON" if on else "OFF", "ok" if on else "idle")
        temp_lbl.setText(f"{_STATE.get('ac_temp', '24')}°C")
        for mid, b in mode_btns.items():
            b.setChecked(_STATE.get("ac_mode", "vent") == mid)
        try:
            fan = int(_STATE.get("ac_fan", 2))
        except (TypeError, ValueError):
            fan = 2
        for lv, b in fan_btns.items():
            b.setChecked(lv == fan)
        carmap.update_state(_STATE)

    ctx.bus.state_changed.connect(_on_state)
    # 初始默认态（与 tool_bus StateManager.h 键一致，避免首屏空值）
    _on_state({"type": "state_full", "values": {
        "ac_on": "false", "ac_temp": "24", "ac_mode": "vent", "ac_fan": "2",
        "window_fl": "0", "window_fr": "0", "window_rl": "0", "window_rr": "0",
        "sunroof_state": "closed", "seat_driver": "0", "seat_passenger": "0"}})
    return page


def _ctl_temp(ctx, d):
    try:
        cur = int(float(_STATE.get("ac_temp", 24)))
    except (TypeError, ValueError):
        cur = 24
    _ctl(ctx, "set_temp", {"temp": max(16, min(30, cur + d))})


def _ctl(ctx, action, payload=None):
    """空调控制：乐观更新 → ctx.tool → 5s 无回读 toast（一次一清，防 toast 积压）。"""
    optimistic = {
        "set_temp": ("ac_temp", lambda p: str(p["temp"])),
        "set_mode": ("ac_mode", lambda p: p["mode"]),
        "set_fan":  ("ac_fan",  lambda p: str(p["level"])),
    }.get(action)
    if optimistic:
        key, fn = optimistic
        _pending[key] = _STATE.get(key)
        val = fn(payload or {})
        _STATE[key] = val
        ctx.bus.state_changed.emit({"type": "state_change", "values": {key: val}})
    ctx.tool("climate_control", action, payload)

    def _rollback():
        if _pending.pop("_armed_" + action, None):
            ctx.toast("空调无应答（tool_bus 离线？）", "warn")
            _pending.clear()

    _pending["_armed_" + action] = True
    QTimer.singleShot(5000, _rollback)


class CarMap(QWidget):
    """车身俯视自绘：四窗/天窗/两座椅为可点区域，开=accent 25% 填充。

    点击映射：window_control open/close（判开→关，否则→开）、sunroof_control
    open/tilt/close 循环、seat_heater driver_on/off。
    """

    def __init__(self, ctx):
        super().__init__()
        self.ctx = ctx
        self.setMinimumHeight(300)
        self.s = {}
        self._zones = {}

    def update_state(self, s):
        self.s = s
        self.update()

    def paintEvent(self, e):
        T = self.window().theme.T
        qp = QPainter(self)
        qp.setRenderHint(QPainter.Antialiasing)

        w, h = self.width(), self.height()
        body = QRectF(w * 0.18, h * 0.10, w * 0.64, h * 0.80)
        path = QPainterPath()
        path.addRoundedRect(body, 36, 36)
        qp.setPen(QPen(QColor(T["card_border"]), 2))
        qp.setBrush(QColor(T["track"]))
        qp.drawPath(path)
        qp.setPen(QColor(T["fg_muted"]))
        qp.setFont(font("Noto Sans CJK SC", 12))
        qp.drawText(QRectF(body.left(), body.top() + 6, body.width(), 20),
                    Qt.AlignCenter, "▲ 前")

        accent = QColor(T["accent"])
        accent.setAlpha(64)
        muted = QColor(T["fg_muted"])

        def zone(key, x, y, bw, bh, on, label, val_text=""):
            r = QRectF(x, y, bw, bh)
            qp.setPen(QPen(QColor(T["accent"]) if on else muted, 2))
            qp.setBrush(accent if on else Qt.NoBrush)
            qp.drawRoundedRect(r, 8, 8)
            qp.setPen(QColor(T["fg"] if on else T["fg_muted"]))
            qp.setFont(font("Noto Sans CJK SC", 13, 500))
            qp.drawText(r, Qt.AlignCenter, label + (f"\n{val_text}" if val_text else ""))
            self._zones[key] = r

        bw, bh = w * 0.17, h * 0.16
        xl, xr = w * 0.235, w * 0.595
        for key, (x, y, name) in {
            "window_fl": (xl, h * 0.19, "左前窗"), "window_fr": (xr, h * 0.19, "右前窗"),
            "window_rl": (xl, h * 0.56, "左后窗"), "window_rr": (xr, h * 0.56, "右后窗"),
        }.items():
            val = int(self.s.get(key, 0) or 0)
            zone(key, x, y, bw, bh, val > 0, name, "开" if val > 0 else "关")
        roof = self.s.get("sunroof_state", "closed")
        zone("sunroof", w * 0.415, h * 0.17, w * 0.17, h * 0.13,
             roof in ("open", "tilted"), "天窗", ROOF_LABEL.get(roof, roof))
        d = int(self.s.get("seat_driver", 0) or 0)
        p = int(self.s.get("seat_passenger", 0) or 0)
        zone("seat_driver", xl, h * 0.74, bw, bh * 0.75, d > 0, "主驾加热",
             f"{d}档" if d else "关")
        zone("seat_passenger", xr, h * 0.74, bw, bh * 0.75, p > 0, "副驾加热",
             f"{p}档" if p else "关")

    def mouseReleaseEvent(self, e):
        pos = e.pos()
        for key, r in self._zones.items():
            if r.contains(pos):
                self._on_zone(key)
                return

    def _on_zone(self, key):
        ctx = self.ctx
        if key.startswith("window_"):
            side = key.split("_")[1]
            open_now = int(self.s.get(key, 0) or 0) > 0
            ctx.tool("window_control", "close" if open_now else "open", {"window": side})
            self.s[key] = 0 if open_now else 100
            ctx.bus.state_changed.emit(
                {"type": "state_change", "values": {key: str(self.s[key])}})
        elif key == "sunroof":
            roof = self.s.get("sunroof_state", "closed")
            nxt = {"closed": ("open", "open"), "open": ("tilt", "tilted"),
                   "tilted": ("close", "closed")}[roof]
            ctx.tool("sunroof_control", nxt[0])
            self.s["sunroof_state"] = nxt[1]
            ctx.bus.state_changed.emit(
                {"type": "state_change", "values": {"sunroof_state": nxt[1]}})
        elif key.startswith("seat_"):
            seat = key.split("_")[1]
            on_now = int(self.s.get(key, 0) or 0) > 0
            ctx.tool("seat_heater", f"{seat}_{'off' if on_now else 'on'}")
            self.s[key] = 0 if on_now else 2
            ctx.bus.state_changed.emit(
                {"type": "state_change", "values": {key: str(self.s[key])}})
