"""settings 插件 — 设置页（主题切换/轮询周期/预览地址/关于）。主题切换落盘 conf。"""

from PyQt5.QtCore import Qt
from PyQt5.QtWidgets import (QButtonGroup, QHBoxLayout, QLabel, QPushButton,
                             QScrollArea, QVBoxLayout, QWidget)

from core.widgets import Card, hline

MANIFEST = {
    "id": "settings", "name": "设置", "icon": "gear-six",
    "order": 40, "tile_span": 1, "page": True,
    "nav_words": ["设置", "设置页面", "系统设置"],
}


def create_card(ctx):
    def _refresh():
        tile = ctx.host.tile_of("settings")
        if tile is not None:
            tile.summary.setText(
                f"{'深色' if ctx.theme.name == 'dark' else '昼间'}主题 · VoxDrive")
    ctx.theme.theme_changed.connect(lambda _: _refresh())
    _refresh()
    return None


def create_page(ctx):
    page = QWidget()
    outer = QVBoxLayout(page)
    outer.setContentsMargins(0, 0, 0, 0)
    body = QWidget()
    bl = QVBoxLayout(body)
    bl.setContentsMargins(0, 0, 0, 0)
    bl.setSpacing(16)

    # ── 外观 ──
    ap = Card()
    bl.addWidget(ap)
    apl = ap.inner
    apl.addWidget(hline("外观", "moon")[0])
    row = QHBoxLayout()
    row.setSpacing(12)
    row.addWidget(QLabel("主题"))
    grp = QButtonGroup(page)
    for name, label in [("dark", "深色"), ("light", "昼间")]:
        b = QPushButton(label)
        b.setMinimumHeight(44)
        b.setMinimumWidth(96)
        b.setCheckable(True)
        b.setChecked(ctx.theme.name == name)
        b.clicked.connect(lambda _, n=name: ctx.theme.set(n))
        grp.addButton(b)
        row.addWidget(b)
    row.addStretch()
    apl.addLayout(row)

    # ── 预览地址（只读展示）──
    pv = Card()
    bl.addWidget(pv)
    pvl = pv.inner
    pvl.addWidget(hline("预览拉流", "eye")[0])
    url = QLabel(ctx.conf.get("dashboard.preview_url", "rtsp://127.0.0.1:8554/live/dashcam"))
    url.setProperty("mono", True)
    url.setObjectName("t_muted")
    pvl.addWidget(url)

    # ── 关于 ──
    ab = Card()
    bl.addWidget(ab)
    abl = ab.inner
    abl.addWidget(hline("关于", "info")[0])
    for line in [
        "VoxDrive 聆行 — 分布式车载智能座舱（RK3588 + Jetson Orin）",
        "语音：ASR zipformer · LLM Qwen2.5 (llama.cpp) · RAG 车主手册 · TTS",
        "行车记录：RK3588 V4L2 → MPP 硬编 → MP4 循环存储 / RTMP 推流",
        f"Dashboard v2.0（插件化宿主 · 深空驾驶舱）",
    ]:
        lb = QLabel(line)
        lb.setObjectName("t_muted")
        lb.setWordWrap(True)
        abl.addWidget(lb)

    bl.addStretch()
    scroll = QScrollArea()
    scroll.setWidgetResizable(True)
    scroll.setWidget(body)
    outer.addWidget(scroll)
    return page
