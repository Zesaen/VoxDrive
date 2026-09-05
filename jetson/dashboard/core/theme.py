"""theme — Deep-Space Cockpit 双主题 token 与 QSS 生成。

设计规范：docs/dashboard_ui_design_spec.md 第 3 节（色板对比度已实测验算，
改动色值必须重跑验算再入库）。昼间/深色同构 token dict，QSS 由模板生成，
禁止在组件里硬编码色值——一律 ctx.theme 取 token。
"""

import os
import pathlib
import re

from PyQt5.QtCore import QObject, pyqtSignal
from PyQt5.QtGui import QColor, QFontDatabase

# ── 色板（语义 token；对比度实测见规范 3.1 表）─────────────────────────

DARK = {
    "bg":         "#0F172A",
    "bg_deep":    "#020617",
    "card":       "rgba(255,255,255,6%)",     # 玻璃卡（合成色须落在 #121A2B–#1C2436 护栏内）
    "card_solid": "#141B2E",                  # 嵌套面板/仪表底（对比度验算口径）
    "card_border": "rgba(255,255,255,12%)",
    "card_hover": "rgba(255,255,255,10%)",
    "fg":         "#F8FAFC",
    "fg_muted":   "#94A3B8",
    "accent":     "#00C8FF",
    "on_accent":  "#062A36",
    "success":    "#22C55E",
    "on_success": "#052E12",
    "warn":       "#FFB020",
    "danger":     "#FF5C5C",
    "rec":        "#FF3B47",
    "btn_bg":     "rgba(255,255,255,8%)",
    "btn_border": "rgba(255,255,255,14%)",
    "hover":      "rgba(255,255,255,10%)",
    "pressed":    "rgba(255,255,255,16%)",
    "disabled_fg": "rgba(248,250,252,40%)",
    "scrim":      "rgba(2,6,23,55%)",
    "track":      "#222B40",                  # 仪表底槽/车体填充（QPainter 用，须 #hex——QColor 不解析 rgba() 串）
}

LIGHT = {
    "bg":         "#EEF2F7",
    "bg_deep":    "#E2E8F0",
    "card":       "#FFFFFF",
    "card_solid": "#F1F5F9",
    "card_border": "#D3DCE6",
    "card_hover": "rgba(15,23,42,4%)",
    "fg":         "#0F172A",
    "fg_muted":   "#475569",
    "accent":     "#00739B",
    "on_accent":  "#FFFFFF",
    "success":    "#15803D",
    "on_success": "#FFFFFF",
    "warn":       "#B45309",
    "danger":     "#DC2626",
    "rec":        "#DC2626",
    "btn_bg":     "#FFFFFF",
    "btn_border": "#C6D2DE",
    "hover":      "rgba(15,23,42,5%)",
    "pressed":    "rgba(15,23,42,10%)",
    "disabled_fg": "rgba(15,23,42,35%)",
    "scrim":      "rgba(226,232,240,70%)",
    "track":      "#DDE5EE",
}

# ── 动效 token（规范 3.4；VOX_DASH_REDUCED_MOTION=1 全部降级为直切）──────

REDUCED_MOTION = os.environ.get("VOX_DASH_REDUCED_MOTION") == "1"
DUR_FAST = 0 if REDUCED_MOTION else 120
DUR_BASE = 0 if REDUCED_MOTION else 200
DUR_SLOW = 0 if REDUCED_MOTION else 320

FONT_SANS = "Noto Sans CJK SC"
FONT_MONO = "DejaVu Sans Mono"  # 注册成功后覆写为 JetBrains Mono

_FONTS_DIR = pathlib.Path(__file__).resolve().parents[1] / "assets" / "fonts"


def register_fonts():
    """捆绑 JetBrains Mono（读数等宽，防数字刷新抖宽）；失败回退 DejaVu Sans Mono。"""
    global FONT_MONO
    loaded = False
    for ttf in sorted(_FONTS_DIR.glob("JetBrainsMono-*.ttf")):
        if QFontDatabase.addApplicationFont(str(ttf)) >= 0:
            loaded = True
    if loaded:
        FONT_MONO = "JetBrains Mono"
    return FONT_MONO


def font(family, size, weight=400):
    return f'font-family: "{family}"; font-size: {size}px; font-weight: {weight};'


# ── QSS 生成 ─────────────────────────────────────────────────────────

def qss(t):
    """全局样式表。t=主题 token dict。组件级微调一律加到这里，不散落 setStyleSheet。"""
    return f"""
    QMainWindow, QWidget#root {{
        background: qlineargradient(y1:0, y2:1, stop:0 {t['bg']}, stop:1 {t['bg_deep']});
    }}
    QFrame#card {{
        background: {t['card']};
        border: 1px solid {t['card_border']};
        border-radius: 16px;
    }}
    QFrame#tile {{
        background: {t['card']};
        border: 1px solid {t['card_border']};
        border-radius: 20px;
    }}
    QFrame#tile:hover {{
        border-color: {t['accent']};
        background: {t['card_hover']};
    }}
    QLabel {{ background: transparent; }}
    QLabel#t_title  {{ color: {t['fg']};       font-size: 20px; font-weight: 500; }}
    QLabel#t_head   {{ color: {t['fg']};       font-size: 16px; font-weight: 500; }}
    QLabel#t_body   {{ color: {t['fg']};       font-size: 14px; }}
    QLabel#t_muted  {{ color: {t['fg_muted']}; font-size: 14px; }}
    QLabel#t_small  {{ color: {t['fg_muted']}; font-size: 12px; }}
    QLabel#t_accent {{ color: {t['accent']};   font-size: 14px; font-weight: 500; }}
    QLabel[mono="true"] {{ font-family: "{FONT_MONO}"; color: {t['fg']}; }}

    QPushButton {{
        background: {t['btn_bg']};
        color: {t['fg']};
        border: 1px solid {t['btn_border']};
        border-radius: 10px;
        padding: 10px 18px;
        font-size: 15px;
        font-weight: 500;
    }}
    QPushButton:hover    {{ background: {t['hover']};  border-color: {t['accent']}; }}
    QPushButton:pressed  {{ background: {t['pressed']}; }}
    QPushButton:focus    {{ border: 2px solid {t['accent']}; }}
    QPushButton:disabled {{ color: {t['disabled_fg']}; border-color: {t['track']}; }}
    QPushButton#primary  {{
        background: {t['accent']}; color: {t['on_accent']};
        border: none; font-weight: bold;
    }}
    QPushButton#primary:hover   {{ background: {t['accent']}; opacity: 1; }}
    QPushButton#rec     {{
        background: {t['rec']}; color: #FFFFFF; border: none; font-weight: bold;
    }}
    QPushButton:checked {{
        background: {t['accent']}; color: {t['on_accent']};
        border-color: {t['accent']}; font-weight: bold;
    }}

    QScrollArea, QScrollArea > QWidget > QWidget {{ background: transparent; border: none; }}
    QScrollBar:vertical {{
        background: transparent; width: 8px; margin: 4px;
    }}
    QScrollBar::handle:vertical {{
        background: {t['track']}; border-radius: 4px; min-height: 40px;
    }}
    QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
    QScrollBar::add-page, QScrollBar::sub-page {{ background: transparent; }}

    QListWidget {{
        background: transparent; border: none; color: {t['fg']};
        font-size: 13px; outline: none;
    }}
    QListWidget::item {{ padding: 3px 2px; border: none; }}
    """


def qss_pill(t):
    """状态 pill（QLabel[pill]，dynamic property state）。"""
    return f"""
    QLabel[pill="true"] {{
        border-radius: 10px; padding: 2px 10px;
        font-size: 12px; font-weight: 500;
        color: {t['fg_muted']}; background: {t['track']};
    }}
    QLabel[pill="true"][state="ok"]     {{ color: {t['success']}; background: {t['track']}; }}
    QLabel[pill="true"][state="warn"]   {{ color: {t['warn']}; }}
    QLabel[pill="true"][state="danger"] {{ color: {t['danger']}; }}
    QLabel[pill="true"][state="accent"] {{ color: {t['accent']}; }}
    QLabel[pill="true"][state="busy"]   {{ color: {t['on_accent']}; background: {t['accent']}; }}
    """


def apply_palette(app, t):
    """主题感知 QPalette（QSS 覆盖不到的角落：菜单、tooltip、占位文本等）。"""
    from PyQt5.QtGui import QPalette
    p = app.palette()
    p.setColor(QPalette.Window, QColor(t["bg"]))
    p.setColor(QPalette.WindowText, QColor(t["fg"]))
    p.setColor(QPalette.Base, QColor(t["card_solid"]))
    p.setColor(QPalette.Text, QColor(t["fg"]))
    p.setColor(QPalette.Button, QColor(t["card_solid"]))
    p.setColor(QPalette.ButtonText, QColor(t["fg"]))
    p.setColor(QPalette.ToolTipBase, QColor(t["card_solid"]))
    p.setColor(QPalette.ToolTipText, QColor(t["fg"]))
    p.setColor(QPalette.Highlight, QColor(t["accent"]))
    p.setColor(QPalette.HighlightedText, QColor(t["on_accent"]))
    app.setPalette(p)


def save_theme_pref(name):
    """主题偏好落盘（settings 页/顶栏切换共用）：改写 conf 的 dashboard.theme 行。"""
    conf = _find_conf()
    if not conf:
        return False
    try:
        text = conf.read_text(encoding="utf-8")
    except OSError:
        return False
    line = f"dashboard.theme = {name}"
    if re.search(r"^dashboard\.theme\s*=.*$", text, flags=re.M):
        text = re.sub(r"^dashboard\.theme\s*=.*$", line, text, count=1, flags=re.M)
    else:
        text += f"\n{line}\n"
    try:
        conf.write_text(text, encoding="utf-8")
        return True
    except OSError:
        return False


def _find_conf():
    """与 common/vox_config.py 同序定位 conf（$VOX_CONF → jetson/config/）。"""
    env = os.environ.get("VOX_CONF")
    cands = [pathlib.Path(env)] if env else []
    cands.append(pathlib.Path(__file__).resolve().parents[2] / "config" / "voxdrive.conf")
    for p in cands:
        if p.is_file():
            return p
    return None


class ThemeManager(QObject):
    """当前主题；切换时发 theme_changed（组件重取 token/重渲染图标）。"""

    theme_changed = pyqtSignal(dict)

    def __init__(self, name="dark"):
        super().__init__()
        self.name = name if name in ("dark", "light") else "dark"
        self._app = None

    def attach_app(self, app):
        self._app = app
        app.setStyleSheet(qss(self.T) + qss_pill(self.T))
        apply_palette(app, self.T)

    @property
    def T(self):
        return DARK if self.name == "dark" else LIGHT

    def toggle(self):
        self.set("light" if self.name == "dark" else "dark")

    def set(self, name):
        if name not in ("dark", "light") or name == self.name:
            return
        self.name = name
        if self._app:
            self._app.setStyleSheet(qss(self.T) + qss_pill(self.T))
            apply_palette(self._app, self.T)
        self.theme_changed.emit(self.T)
        save_theme_pref(name)
