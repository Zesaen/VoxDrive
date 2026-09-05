"""dashcam 插件 — 行车记录（预览/控制/状态/事件日志 + 主页 RK 快览条）。

数据：ctx.bus.dashcam_status（:6700 轮询）/ rk_event（:6701 事件）/ frame+stream_state（预览）。
控制：ctx.tool("dashcam", action)（与语音同走 tool_bus → RK）。
"""

import datetime
import json

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QPixmap
from PyQt5.QtWidgets import (QGridLayout, QHBoxLayout, QLabel, QListWidget,
                             QListWidgetItem, QPushButton, QScrollArea,
                             QVBoxLayout, QWidget)

from core import icons
from core.theme import FONT_MONO
from core.widgets import ArcGauge, Card, font, hline, set_pill

ALERTS = {
    "watermark_deleted":   ("warn",   "存储超水位：已自动删除最旧录像段"),
    "write_error":         ("danger", "录像写入异常，正在恢复"),
    "capture_timeout":     ("danger", "摄像头采集超时，请检查行车记录仪"),
    "rtmp_disconnected":   ("warn",   "预览推流中断，自动重连中"),
    "rtmp_connect_failed": ("warn",   "预览推流连接失败，稍后自动重试"),
    "detect":              ("info",   "检测到行人/车辆，当前录像段已锁定保护"),
    "lock_released":       ("info",   "锁定段超出保护配额，已释放最旧锁定段"),
}

MANIFEST = {
    "id": "dashcam", "name": "行车记录", "icon": "video-camera",
    "order": 10, "tile_span": 2, "page": True,
    "nav_words": ["行车记录", "记录仪", "行车记录页面", "录像页面"],
}


def create_card(ctx):
    """主页磁贴内容（宿主只给网格位置，插件自喂摘要）。"""
    def _on_status(p):
        tile = ctx.host.tile_of("dashcam")
        if tile is None:
            return
        if p is None:
            tile.summary.setText("RK 离线")
            return
        st = p.get("storage", {})
        rec = "录像中" if p.get("recording") else "已暂停"
        tile.summary.setText(
            f"{rec} · {p.get('pipeline_fps', 0):.0f}fps · 磁盘 {st.get('used_percent', 0):.0f}%")

    ctx.bus.dashcam_status.connect(_on_status)
    ctx.bus.dashcam_status.emit(ctx.store.get("rk_status"))
    return None  # 磁贴主体由宿主按 MANIFEST 画，插件只喂摘要


def create_home_strip(ctx):
    """主页顶部 RK 快览条（h64 卡片，点击→dashcam 页）。"""
    bar = Card()
    bar.setFixedHeight(64)
    lay = bar.inner              # Card 自带布局（再装一个 QHBoxLayout 会互相顶掉）
    from PyQt5.QtWidgets import QBoxLayout
    lay.setDirection(QBoxLayout.LeftToRight)  # Card 默认纵向，快览条需横向
    lay.setContentsMargins(20, 8, 20, 8)
    lay.setSpacing(12)
    ic = icons.IconLabel("video-camera", 24, "accent")
    lay.addWidget(ic)

    state = QLabel("…")
    state.setObjectName("t_head")
    fps = QLabel("-- fps")
    fps.setObjectName("t_body")
    fps.setProperty("mono", True)
    segs = QLabel("-- 段")
    segs.setObjectName("t_muted")
    free = QLabel("-- GB 可用")
    free.setObjectName("t_muted")
    pill = QLabel("RK 离线")
    pill.setProperty("pill", True)
    lay.addWidget(state)
    lay.addWidget(fps)
    lay.addWidget(segs)
    lay.addWidget(free)
    lay.addStretch()
    lay.addWidget(pill)
    bar.setCursor(Qt.PointingHandCursor)
    bar.mousePressEvent = lambda e: ctx.navigate("dashcam")

    def _on(p):
        if p is None:
            set_pill(pill, "RK 离线", "danger")
            state.setText("行车记录仪离线")
            fps.setText("-- fps")
            segs.setText("-- 段")
            free.setText("-- GB")
            return
        st = p.get("storage", {})
        rec = bool(p.get("recording"))
        state.setText("● 录像中" if rec else "○ 已暂停")
        fps.setText(f"{p.get('pipeline_fps', 0):.1f} fps")
        segs.setText(f"{st.get('segments_total', 0)} 段")
        free.setText(f"{st.get('free_gb', 0):.1f} GB 可用")
        set_pill(pill, "在线", "ok")

    ctx.bus.dashcam_status.connect(_on)
    ctx.bus.dashcam_status.emit(ctx.store.get("rk_status"))
    return bar


def create_page(ctx):
    page = QWidget()
    root = QHBoxLayout(page)
    root.setContentsMargins(0, 0, 0, 0)
    root.setSpacing(16)

    # ── 左：预览玻璃卡（62%）──
    left = Card()
    root.addWidget(left, 62)
    ll = left.inner

    live_pill = QLabel("○ 预览关")
    live_pill.setProperty("pill", True)
    head, _ = hline("行车记录 · 实时预览", "video-camera", live_pill)
    ll.addWidget(head)

    from PyQt5.QtWidgets import QFrame
    cam = QLabel("预览未开启")
    cam.setAlignment(Qt.AlignCenter)
    cam.setStyleSheet("background:#000; border-radius:14px;")
    cam.setMinimumHeight(420)
    cam.setScaledContents(True)
    ll.addWidget(cam, 1)

    btns = QHBoxLayout()
    btns.setSpacing(12)
    rec_btn = QPushButton("● REC")
    rec_btn.setMinimumHeight(48)
    pv_btn = QPushButton("预览 开")
    pv_btn.setMinimumHeight(48)
    snap_btn = QPushButton("抓拍")
    snap_btn.setMinimumHeight(48)
    btns.addWidget(rec_btn)
    btns.addWidget(pv_btn)
    btns.addWidget(snap_btn)
    btns.addStretch()
    ll.addLayout(btns)

    def _on_stream_state(s):
        if s == "live":
            set_pill(live_pill, "● LIVE", "ok")
        elif s == "retry":
            set_pill(live_pill, "● 重连中", "warn")
            cam.setPixmap(QPixmap())
            cam.setText("信号中断，重连中…")
        else:
            set_pill(live_pill, "○ 预览关", "idle")
            cam.setPixmap(QPixmap())
            cam.setText("预览未开启")

    def _on_frame(img):
        cam.setText("")
        cam.setPixmap(QPixmap.fromImage(img))

    ctx.bus.stream_state.connect(_on_stream_state)
    ctx.bus.frame.connect(_on_frame)

    # ── 右：状态卡 + 事件日志（38%）──
    right = QVBoxLayout()
    right.setSpacing(16)
    root.addLayout(right, 38)

    st_card = Card()
    right.addWidget(st_card)
    sl = st_card.inner
    st_state = QLabel("连接中…")
    st_state.setProperty("pill", True)
    sl.addWidget(hline("行车记录仪", "record", st_state)[0])
    row = QHBoxLayout()
    fps_l = QLabel("-- fps")
    fps_l.setObjectName("t_accent")
    fps_l.setProperty("mono", True)
    seg_l = QLabel("-- 段")
    seg_l.setObjectName("t_muted")
    seg_l.setProperty("mono", True)
    free_l = QLabel("-- GB")
    free_l.setObjectName("t_muted")
    free_l.setProperty("mono", True)
    row.addWidget(fps_l)
    row.addStretch()
    row.addWidget(seg_l)
    row.addStretch()
    row.addWidget(free_l)
    sl.addLayout(row)
    gauge = ArcGauge()
    sl.addWidget(gauge, 0, Qt.AlignHCenter)

    def _on_status(p):
        if p is None:
            st_state.setText("离线")
            set_pill(st_state, "离线", "danger")
            fps_l.setText("-- fps")
            seg_l.setText("-- 段")
            free_l.setText("-- GB")
            gauge.set_val(0)
            return
        rec = bool(p.get("recording"))
        set_pill(st_state, "● 录像中" if rec else "○ 暂停", "ok" if rec else "warn")
        st = p.get("storage", {})
        fps_l.setText(f"{p.get('pipeline_fps', 0):.1f} fps")
        seg_l.setText(f"{st.get('segments_total', 0)} 段")
        free_l.setText(f"{st.get('free_gb', 0):.1f} GB")
        gauge.set_val(st.get("used_percent", 0.0))

    ctx.bus.dashcam_status.connect(_on_status)
    ctx.bus.dashcam_status.emit(ctx.store.get("rk_status"))

    log_card = Card()
    right.addWidget(log_card, 1)
    log_l = log_card.inner
    log_l.addWidget(hline("事件日志", "pulse")[0])
    log = QListWidget()
    log_l.addWidget(log, 1)

    def _on_rk_event(m):
        p = m.get("payload", {}) if isinstance(m, dict) else {}
        if not isinstance(p, dict) or not p.get("event"):
            return
        ev = p["event"]
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        item = QListWidgetItem(f"{ts}  {ev}")
        log.insertItem(0, item)
        while log.count() > 20:
            log.takeItem(log.count() - 1)
        spec = ALERTS.get(ev)
        if spec:
            level, text = spec
            ctx.toast(f"{text}（{ts}）", level)

    ctx.bus.rk_event.connect(_on_rk_event)

    # ── 控制动作（与语音同路 tool_bus；命令后 poke 立即回读）─────────────
    def _done(_r):
        ctx.poller.poke()

    def toggle_rec():
        st = ctx.store.get("rk_status") or {}
        on = st.get("online") and not st.get("recording")
        ctx.tool("dashcam", "record_on" if on else "record_off", on_done=_done)
        ctx.poller.poke()

    def toggle_preview():
        on = not ctx.player.enabled
        ctx.player.set_enabled(on)
        pv_btn.setText("预览 开" if on else "预览 关")
        ctx.tool("dashcam", "preview_on" if on else "preview_off", on_done=_done)
        if not on:
            set_pill(live_pill, "○ 预览关", "idle")
            cam.setPixmap(QPixmap())
            cam.setText("预览未开启")

    def snap():
        ctx.tool("dashcam", "snapshot", on_done=_done)
        ctx.toast("抓拍已请求，JPEG 落盘 ~/voxdrive_snapshots/", "info")

    rec_btn.clicked.connect(toggle_rec)
    pv_btn.clicked.connect(toggle_preview)
    snap_btn.clicked.connect(snap)

    def _sync_buttons(p):
        rec = bool(p.get("recording")) if p else False
        rec_btn.setText("● 停止录像" if rec else "● 开始录像")
        pv = getattr(ctx, "player", None)
        pv_btn.setText("预览 关" if (pv is not None and pv.enabled) else "预览 开")

    ctx.bus.dashcam_status.connect(_sync_buttons)
    return page
