"""status 插件 — 状态页（传感器 sensor_read 喂活 + 服务健康 + RK 存储详情）。"""

import json

from PyQt5.QtWidgets import QHBoxLayout, QLabel, QScrollArea, QVBoxLayout, QWidget

from core.widgets import Card, hline, set_pill

MANIFEST = {
    "id": "status", "name": "状态", "icon": "hard-drive",
    "order": 30, "tile_span": 1, "page": True,
    "nav_words": ["状态", "状态页面", "传感器页面"],
}

SENSOR_LABELS = [
    ("speed", "车速", "km/h"), ("fuel_percent", "油量", "%"),
    ("tire_fl", "胎压·左前", "bar"), ("tire_fr", "胎压·右前", "bar"),
    ("tire_rl", "胎压·左后", "bar"), ("tire_rr", "胎压·右后", "bar"),
    ("engine_temp", "水温", "°C"), ("battery_voltage", "电瓶", "V"),
    ("odometer", "里程", "km"),
]


def create_card(ctx):
    def _refresh():
        tile = ctx.host.tile_of("status")
        d = ctx.store.get("sensor", {})
        if tile is not None:
            tile.summary.setText(
                f"车速 {d.get('speed', '--')}km/h · 油量 {d.get('fuel_percent', '--')}%")
    ctx.poll(int(ctx.conf.get_int("dashboard.sensor_poll_s", 10) * 1000), _poll_once(ctx, _refresh))
    _refresh()
    return None


def _poll_once(ctx, refresh):
    def _run():
        def _done(r):
            try:
                d = json.loads(r.get("result", "{}")) if isinstance(r, dict) else {}
            except (json.JSONDecodeError, ValueError, AttributeError):
                d = {}
            ctx.store["sensor"] = d
            refresh()
        ctx.tool("sensor_read", "read", on_done=_done)
    return _run


def create_page(ctx):
    page = QWidget()
    outer = QVBoxLayout(page)
    outer.setContentsMargins(0, 0, 0, 0)
    body = QWidget()
    bl = QVBoxLayout(body)
    bl.setContentsMargins(0, 0, 0, 0)
    bl.setSpacing(16)

    # ── 传感器卡（grid 双列，喂活原死元素）────────────────────────────
    sen = Card()
    bl.addWidget(sen)
    sl = sen.inner
    sl.addWidget(hline("传感器（sensor_read）", "activity_placeholder" if False else "pulse")[0])
    grid = QHBoxLayout()
    grid.setSpacing(24)
    sen_cells = {}

    def _make_cell():
        w = QWidget()
        c = QVBoxLayout(w)
        c.setContentsMargins(0, 0, 0, 0)
        c.setSpacing(2)
        v = QLabel("--")
        v.setProperty("mono", True)
        v.setStyleSheet("font-size: 22px; font-weight: 500;")
        k = QLabel("")
        k.setObjectName("t_small")
        c.addWidget(v)
        c.addWidget(k)
        return w, v, k

    for key, name, unit in SENSOR_LABELS:
        w, v, k = _make_cell()
        k.setText(f"{name} ({unit})")
        grid.addWidget(w)
        sen_cells[key] = v
    bl_w = QWidget()
    bl_w.setLayout(grid)
    sl.addWidget(bl_w)

    def _show_sensor():
        d = ctx.store.get("sensor", {})
        if not d:
            for v in sen_cells.values():
                v.setText("--")
            return
        for key, v in sen_cells.items():
            val = d.get(key, "--")
            v.setText(str(val))

    # ── RK 存储详情卡 ──
    rk = Card()
    bl.addWidget(rk)
    rl = rk.inner
    rk_pill = QLabel("…")
    rk_pill.setProperty("pill", True)
    rl.addWidget(hline("RK 行车记录仪详情", "record", rk_pill)[0])
    rk_rows = {}
    for key, label in [("segments_total", "段总数"), ("segments_deleted", "已删段（水位覆盖）"),
                       ("bytes_written", "累计写入"), ("current_file", "当前段文件"),
                       ("frames_sent", "推流帧数"), ("connect_failures", "推流失败计数")]:
        row = QHBoxLayout()
        k = QLabel(label)
        k.setObjectName("t_muted")
        v = QLabel("--")
        v.setProperty("mono", True)
        row.addWidget(k)
        row.addStretch()
        row.addWidget(v)
        rl.addLayout(row)
        rk_rows[key] = v

    def _on_rk(p):
        if p is None:
            set_pill(rk_pill, "离线", "danger")
            return
        set_pill(rk_pill, "在线", "ok")
        st = p.get("storage", {})
        rt = p.get("rtmp", {})
        mapping = {"segments_total": st.get("segments_total"),
                   "segments_deleted": st.get("segments_deleted"),
                   "bytes_written": f"{st.get('bytes_written', 0) / 1e9:.2f} GB",
                   "current_file": st.get("current_file", "--").split("/")[-1],
                   "frames_sent": rt.get("frames_sent"),
                   "connect_failures": rt.get("connect_failures")}
        for k2, val in mapping.items():
            if k2 in rk_rows:
                rk_rows[k2].setText(str(val) if val is not None else "--")

    ctx.bus.dashcam_status.connect(_on_rk)
    ctx.bus.dashcam_status.emit(ctx.store.get("rk_status"))

    bl.addStretch()
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setWidget(body)
    outer.addWidget(scroll)

    _poll_once(ctx, _show_sensor)()  # 首轮立即拉
    return page
