"""widgets — 公共组件（设计规范 6 节规格）。

组件从 ctx.theme 取 token，不硬编码色值；主题切换重绘。
"""

from PyQt5.QtCore import (QEasingCurve, QPointF, QRectF, Qt, QTimer,  # noqa: F401
                          QVariantAnimation, pyqtSignal)
from PyQt5.QtGui import QColor, QFont, QPainter, QPainterPath, QPen
from PyQt5.QtWidgets import (QFrame, QGraphicsOpacityEffect, QHBoxLayout, QLabel,
                             QPushButton, QSizePolicy, QVBoxLayout, QWidget)

from core import icons
from core.theme import DUR_BASE, DUR_FAST, FONT_MONO, FONT_SANS


def font(family, size, weight=400):
    f = QFont(family)
    f.setPixelSize(size)
    f.setWeight(QFont.Normal if weight <= 400 else
                QFont.DemiBold if weight <= 600 else QFont.Bold)
    return f


def set_pill(lbl, text, state="idle"):
    """状态 pill：dynamic property 驱动 QSS（theme.qss_pill）。"""
    lbl.setText(text)
    lbl.setFixedHeight(28)  # 布局拉伸时 pill 不纵向变形（车机观感硬约束）
    lbl.setProperty("state", state)
    lbl.style().unpolish(lbl)
    lbl.style().polish(lbl)


def hline(title, icon_name=None, extra=None):
    """卡片标题行：图标 + 标题 + stretch + extra。返回 (widget, iconlabel)。"""
    w = QWidget()
    lay = QHBoxLayout(w)
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(8)
    ic = None
    if icon_name:
        ic = icons.IconLabel(icon_name, 20, "accent")
        lay.addWidget(ic)
    t = QLabel(title)
    t.setObjectName("t_head")
    lay.addWidget(t)
    lay.addStretch()
    if extra is not None:
        lay.addWidget(extra, 0, Qt.AlignVCenter)
    return w, ic


class Card(QFrame):
    """玻璃卡容器：Card(children=..., title=..., icon=...)。"""

    def __init__(self, parent=None, inner=None):
        super().__init__(parent)
        self.setObjectName("card")
        self._lay = QVBoxLayout(self)
        self._lay.setContentsMargins(20, 16, 20, 16)
        self._lay.setSpacing(10)
        if inner is not None:
            lay = QVBoxLayout(inner)
            lay.setContentsMargins(0, 0, 0, 0)
            self._lay.addLayout(lay)
            self.inner = lay
        else:
            self.inner = self._lay


class Tile(QFrame):
    """主页磁贴：图标 + 名称 + 摘要行；点击 → navigate。摘要须是活数据（插件自喂）。"""

    clicked = pyqtSignal(str)

    def __init__(self, plugin, ctx, parent=None):
        super().__init__(parent)
        self.setObjectName("tile")
        self.plugin = plugin
        self.ctx = ctx
        self.setCursor(Qt.PointingHandCursor)
        self.setMinimumHeight(170)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(24, 20, 24, 20)
        lay.setSpacing(10)
        ic = icons.IconLabel(plugin.icon, 32, "accent")
        lay.addWidget(ic)
        lay.addStretch()
        nm = QLabel(plugin.name)
        nm.setObjectName("t_title")
        lay.addWidget(nm)
        self.summary = QLabel("")
        self.summary.setObjectName("t_muted")
        lay.addWidget(self.summary)

    def mouseReleaseEvent(self, e):
        if e.button() == Qt.LeftButton:
            self.clicked.emit(self.plugin.id)


class ArcGauge(QWidget):
    """存储水位弧形仪表：数字+文字并排（不靠色独达，规范 6 节）。值弧色随阈值。

    用 QVariantAnimation 而非 QPropertyAnimation：后者回写属性会重入 set_val
    （set_val 内部再 start）→ 无限递归段错误（2026-09-05 实测踩坑）。
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(132, 110)
        self._val = 0.0
        self._anim = QVariantAnimation(self)
        self._anim.setDuration(DUR_BASE)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)
        self._anim.valueChanged.connect(self._on_val)

    def _on_val(self, v):
        self._val = float(v)
        self.update()

    def set_val(self, v):
        self._anim.stop()
        self._anim.setStartValue(self._val)
        self._anim.setEndValue(float(v))
        self._anim.start()

    def paintEvent(self, e):
        T = self.window().theme.T if hasattr(self.window(), "theme") else {}
        if not T:
            return
        qp = QPainter(self)
        qp.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        side = min(w, h * 1.2)
        rect = QRectF((w - side) / 2, (h - side) / 2, side, side)
        start, span = 225 * 16, -270 * 16
        pen = QPen(QColor(T.get("track", "#333")), 10)
        pen.setCapStyle(Qt.RoundCap)
        qp.setPen(pen)
        qp.drawArc(rect, start, span)
        pct = max(0.0, min(self._val / 100.0, 1.0))
        if pct > 0:
            c = T["success"] if self._val < 75 else T["warn"] if self._val < 90 else T["danger"]
            pen.setColor(QColor(c))
            qp.setPen(pen)
            qp.drawArc(rect, start, int(span * pct))
        qp.setPen(QColor(T["fg"]))
        qp.setFont(font(FONT_MONO, 26, 700))
        qp.drawText(QRectF(0, h * 0.34, w, 34), Qt.AlignCenter, f"{int(self._val)}%")
        qp.setPen(QColor(T["fg_muted"]))
        qp.setFont(font(FONT_SANS, 12))
        qp.drawText(QRectF(0, h * 0.66, w, 20), Qt.AlignCenter, "磁盘已用")


class WaveBar(QWidget):
    """语音波形：12 根圆角竖条。idle 静止 / listening accent / speaking success。

    无限循环动画仅此处（"活动指示"允许项，规范 3.4）；reduced-motion 直切静条。
    30fps 上限定时器；不可见时自动停表（CPU 纪律）。
    """

    MODE_IDLE, MODE_LISTEN, MODE_SPEAK, MODE_THINK = range(4)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(96, 44)
        self.mode = self.MODE_IDLE
        self._phase = 0.0
        self._timer = QTimer(self)
        self._timer.timeout.connect(self._tick)
        self._timer.setInterval(33)

    def set_mode(self, mode):
        self.mode = mode
        if mode == self.MODE_IDLE:
            self._timer.stop()
            self.update()
        else:
            if not self._timer.isActive():
                self._timer.start()

    def _tick(self):
        self._phase += 0.35
        self.update()

    def paintEvent(self, e):
        T = self.window().theme.T if hasattr(self.window(), "theme") else {}
        if not T:
            return
        qp = QPainter(self)
        qp.setRenderHint(QPainter.Antialiasing)
        if self.mode == self.MODE_IDLE:
            color = QColor(T["fg_muted"])
            color.setAlpha(70)
        else:
            color = QColor(T["accent"] if self.mode == self.MODE_LISTEN else
                           T["success"] if self.mode == self.MODE_SPEAK else T["warn"])
        qp.setPen(Qt.NoPen)
        qp.setBrush(color)
        n, bw, gap = 12, 4, 4
        x = (self.width() - (n * bw + (n - 1) * gap)) / 2
        cy = self.height() / 2
        import math
        for i in range(n):
            if self.mode == self.MODE_IDLE:
                hh = 3.0
            elif self.mode == self.MODE_THINK:
                hh = 4 + 8 * (0.5 + 0.5 * math.sin(self._phase * 2 + i * 0.8))
            else:
                env = 0.5 + 0.5 * math.sin(self._phase + i * 0.55)
                hh = 4 + 30 * env
            qp.drawRoundedRect(QRectF(x, cy - hh / 2, bw, hh), 2, 2)
            x += bw + gap


class AlertToast(QFrame):
    """顶部居中 toast：info/warn/danger 三级，8s 自动收回，滑入滑出。"""

    closed = pyqtSignal(object)

    def __init__(self, text, level="info", parent=None):
        super().__init__(parent)
        T = parent.theme.T if parent is not None and hasattr(parent, "theme") else {}
        color = T.get({"info": "accent", "warn": "warn", "danger": "danger"}[level], "#09f")
        self.setStyleSheet(
            f"QFrame {{ background: {T.get('card_solid', '#141B2E')};"
            f" border: 1px solid {T.get('card_border', '#333')};"
            f" border-left: 4px solid {color}; border-radius: 12px; }}")
        lay = QHBoxLayout(self)
        lay.setContentsMargins(16, 10, 16, 10)
        ic = icons.IconLabel({"info": "info", "warn": "warning", "danger": "warning"}[level],
                             20, color if color.startswith("#") else None)
        if not color.startswith("#"):
            ic.setColor(color)
        lay.addWidget(ic)
        t = QLabel(text)
        t.setObjectName("t_body")
        lay.addWidget(t)
        lay.addStretch()
        self._eff = QGraphicsOpacityEffect(self)
        self.setGraphicsEffect(self._eff)
        self._eff.setOpacity(0.0)
        self._anim = QPropertyAnimation(self._eff, b"opacity", self)
        self._anim.setDuration(DUR_SLOW)
        self._anim.setEasingCurve(QEasingCurve.OutCubic)
        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self.dismiss)

    def show_toast(self):
        self._anim.stop()
        self._anim.setStartValue(0.0)
        self._anim.setEndValue(1.0)
        self._anim.start()
        self._timer.start(8000)

    def dismiss(self):
        self._anim.stop()
        self._anim.setStartValue(1.0)
        self._anim.setEndValue(0.0)
        try:
            self._anim.finished.disconnect()
        except TypeError:
            pass
        self._anim.finished.connect(self._gone)
        self._anim.start()

    def _gone(self):
        self.hide()
        self.closed.emit(self)


class IconButton(QPushButton):
    """图标按钮（顶栏主题切换/dock）：44-56px 触控目标。"""

    def __init__(self, icon_name, tooltip="", parent=None):
        super().__init__(parent)
        self.icon_name = icon_name
        self.setFixedSize(44, 44)
        self.setToolTip(tooltip)
        self.setCheckable(True)
        self._refresh()

    def setName(self, icon_name):
        self.icon_name = icon_name
        self._refresh()

    def _refresh(self):
        T = self.window().theme.T if hasattr(self.window(), "theme") else {}
        c = T.get("fg_muted", "#888")
        self.setIcon(icons.icon(self.icon_name, 22, c))
        self.setIconSize(self.size() * 0.5)

    def showEvent(self, e):
        self._refresh()
        super().showEvent(e)
