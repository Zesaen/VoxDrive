"""icons — SVG 图标加载（QtSvg）与主题色重染。

资产：assets/icons/*.svg（Phosphor Icons regular，MIT，复制不软链）。
SVG 以 currentColor 渲染为黑→按 alpha 掩膜染成任意主题色；主题切换时
语义色键名的图标自动重渲染。
"""

import pathlib

from PyQt5.QtCore import Qt
from PyQt5.QtGui import QColor, QIcon, QPainter, QPixmap
from PyQt5.QtSvg import QSvgRenderer
from PyQt5.QtWidgets import QLabel

_ICONS_DIR = pathlib.Path(__file__).resolve().parents[1] / "assets" / "icons"
_renderers = {}
_current_theme = None
_LIVE = set()


def renderer(name):
    if name not in _renderers:
        path = _ICONS_DIR / f"{name}.svg"
        _renderers[name] = QSvgRenderer(str(path)) if path.is_file() else None
    return _renderers[name]


def pixmap(name, size=24, color=None):
    """渲染 SVG → 指定颜色（None=原生黑）的方形 QPixmap。"""
    pm = QPixmap(size, size)
    pm.fill(Qt.transparent)
    r = renderer(name)
    if r is None or not r.isValid():
        return pm
    p = QPainter(pm)
    r.render(p)
    p.end()
    if color is not None:
        tinted = QPixmap(size, size)
        tinted.fill(QColor(color))
        p.begin(tinted)
        p.setCompositionMode(QPainter.CompositionMode_DestinationIn)
        p.drawPixmap(0, 0, pm)
        p.end()
        pm = tinted
    return pm


def icon(name, size=24, color=None):
    return QIcon(pixmap(name, size, color))


def resolve_color(color, T=None):
    """颜色入口：'#hex'/'rgba…' 原样；语义键名（accent/success/…）查主题 token。"""
    if color is None:
        return None
    if color.startswith("#") or color.startswith("rgba"):
        return color
    return (T or _current_theme or {}).get(color, color)


def set_theme(T):
    """宿主在主题切换时调用：刷新语义色缓存并通知所有 IconLabel 重渲染。"""
    global _current_theme
    _current_theme = T
    for w in list(_LIVE):
        w.refresh()


class IconLabel(QLabel):
    """主题感知图标：IconLabel('video-camera', 24, 'accent')。色用语义键则随主题切换。"""

    def __init__(self, name, size=24, color=None, parent=None):
        super().__init__(parent)
        self._name = name
        self._size = size
        self._color = color
        self.setFixedSize(size, size)
        _LIVE.add(self)
        self.refresh()

    def setName(self, name):
        self._name = name
        self.refresh()

    def setColor(self, color):
        self._color = color
        self.refresh()

    def refresh(self):
        self.setPixmap(pixmap(self._name, self._size,
                              resolve_color(self._color)))

    def hideEvent(self, e):
        _LIVE.discard(self)
        super().hideEvent(e)

    def showEvent(self, e):
        _LIVE.add(self)
        super().showEvent(e)
