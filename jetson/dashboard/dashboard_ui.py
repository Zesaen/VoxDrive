#!/usr/bin/env python3
"""数字座舱 Dashboard v2.0 — 插件化宿主（Deep-Space Cockpit）。

设计规范：docs/dashboard_ui_design_spec.md（UI 改动前先读它）。
宿主只做五件事：加载插件 / 路由页面 / 渲染持久层（顶栏·底栏 dock·toast）/
分发事件（core.ctx）/ 主题管理。业务全部在 plugins/*.py。

数据面：core.ctx.start_sources（ZmqHub 四路 SUB + DashcamPoller + StreamPlayer）。
语音区：asr_final/tts_say（intent_router 6671 扩展事件）+ play_end（6678）四态状态机。

自测钩子（无屏验证，保留自 v1）：
  VOX_DASH_SNAPSHOT=/tmp/x.png [VOX_DASH_SNAPSHOT_S=N] [VOX_DASH_SNAPSHOT_PAGE=id]
  VOX_DASH_AUTOPREVIEW=1  VOX_DASH_REDUCED_MOTION=1
"""

import datetime
import os
import pathlib
import sys

JETSON_ROOT = pathlib.Path(__file__).resolve().parents[1]  # dashboard/ → jetson/
sys.path.insert(0, str(JETSON_ROOT))
_DASH_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(_DASH_DIR))  # core/ 相对 import

# 解释器护栏：zmq/PyQt5 装在系统 python3（apt），conda 环境自动重 exec
try:
    import zmq  # noqa: F401,E402
    import PyQt5  # noqa: F401,E402
except ModuleNotFoundError:
    if os.environ.pop("_VOX_SYS_PY", None) is None:
        os.environ["_VOX_SYS_PY"] = "1"
        os.execv("/usr/bin/python3",
                 ["/usr/bin/python3", str(pathlib.Path(__file__).resolve())] + sys.argv[1:])
    raise

from PyQt5.QtCore import QTimer  # noqa: E402
from PyQt5.QtWidgets import (QApplication, QFrame, QHBoxLayout, QLabel,  # noqa: E402
                             QMainWindow, QScrollArea, QStackedWidget,
                             QVBoxLayout, QWidget)

from common import vox_config  # noqa: E402
from core import ctx as ctx_mod  # noqa: E402
from core import icons, loader  # noqa: E402
from core.theme import ThemeManager, register_fonts  # noqa: E402
from core.widgets import IconButton, Tile, AlertToast, WaveBar, set_pill  # noqa: E402

VOICE_IDLE_MS = 8000  # 语音字幕/气泡无新事件淡出窗口（规范 7.4）


class VoiceArea(QWidget):
    """底栏右段语音区：状态徽章 + 字幕/气泡 + 波形（四态状态机，规范 7 节）。"""

    def __init__(self, theme_mgr, parent=None):
        super().__init__(parent)
        self.theme = theme_mgr
        self.setMinimumHeight(88)
        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 4, 8, 4)
        lay.setSpacing(12)

        self.badge = QLabel("聆行待命")
        self.badge.setProperty("pill", True)
        self.badge.setFixedHeight(28)
        lay.addWidget(self.badge)

        self.caption = QLabel("说「打开行车记录」试试")
        self.caption.setObjectName("t_muted")
        self.caption.setMinimumWidth(320)
        lay.addWidget(self.caption, 1)

        self.wave = WaveBar()
        lay.addWidget(self.wave)

        self._fade = QTimer(self)
        self._fade.setSingleShot(True)
        self._fade.timeout.connect(lambda: self._set("idle", ""))
        self._set("idle", "")

    def _set(self, state, text):
        """idle / listening / thinking / speaking（规范 7.1）。"""
        self.caption.setText(text)
        if state == "idle":
            set_pill(self.badge, "聆行待命", "idle")
            self.wave.set_mode(WaveBar.MODE_IDLE)
        elif state == "listening":
            set_pill(self.badge, "聆听中", "accent")
            self.wave.set_mode(WaveBar.MODE_LISTEN)
        elif state == "thinking":
            set_pill(self.badge, "思考中", "busy")
            self.wave.set_mode(WaveBar.MODE_THINK)
        elif state == "speaking":
            set_pill(self.badge, "播报中", "ok")
            self.wave.set_mode(WaveBar.MODE_SPEAK)

    def on_asr(self, text):
        print(f"[voice] asr_final: {text}", flush=True)
        self._set("listening", "你：" + text)
        self._fade.stop()
        # asr_final 即路由开始处理 → thinking（同事件落定，规范 7.1）
        QTimer.singleShot(600, lambda: self._set("thinking", "你：" + text))

    def on_tts(self, text):
        print(f"[voice] tts_say: {text}", flush=True)
        self._fade.stop()
        self._set("speaking", "聆行：" + text)

    def on_play_end(self):
        self._fade.start(VOICE_IDLE_MS)


class Dashboard(QMainWindow):
    """宿主窗口：顶栏 / 页面路由(QStackedWidget) / 底栏(dock+语音区) / toast 层。"""

    def __init__(self, app):
        super().__init__()
        self.app = app
        self.setWindowTitle("聆行 VoxDrive · 数字座舱")
        self.theme = ThemeManager(vox_config.get("dashboard.theme", "dark"))
        self.ctx = ctx_mod.PluginContext(self.theme, self)
        self._toast_queue = []

        register_fonts()
        self._build_plugins()
        icons.set_theme(self.theme.T)  # 首屏语义色图标（切换时 _on_theme 再刷）
        self._ui()
        self._voice()
        self._sources()
        self.theme.attach_app(app)
        self.theme.theme_changed.connect(self._on_theme)
        self.ctx.bus.service_status.connect(self._on_service_status)
        self.clock = QTimer(self)
        self.clock.timeout.connect(self._tick)
        self.clock.start(1000)
        self._tick()
        self.resize(1600, 900)
        # VOX_DASH_FULLSCREEN=1 强制最大化；屏幕小于设计尺寸（如 RK 触摸屏 1024x600）时也自动最大化
        geo = QApplication.primaryScreen().availableGeometry() if QApplication.primaryScreen() else None
        if os.environ.get("VOX_DASH_FULLSCREEN") == "1" or (
                geo is not None and (geo.width() < 1600 or geo.height() < 900)):
            self.showMaximized()

    # ── 插件装配 ─────────────────────────────────────────────────────
    def _build_plugins(self):
        self.plugins = loader.scan()
        print("[host] plugins: "
              + ", ".join(f"{p.id}(order={p.order})" for p in self.plugins), flush=True)

    def tile_of(self, plugin_id):
        return self._tiles.get(plugin_id)

    # ── 持久层 ───────────────────────────────────────────────────────
    def _ui(self):
        root = QWidget()
        root.setObjectName("root")
        self.setCentralWidget(root)
        rl = QVBoxLayout(root)
        rl.setContentsMargins(0, 0, 0, 0)
        rl.setSpacing(0)
        rl.addWidget(self._topbar())
        rl.addWidget(self._content(), 1)
        rl.addWidget(self._bottombar())

    def _topbar(self):
        bar = QFrame()
        bar.setFixedHeight(64)
        lay = QHBoxLayout(bar)
        lay.setContentsMargins(24, 8, 24, 8)
        lay.setSpacing(12)

        self.clk = QLabel()
        self.clk.setProperty("mono", True)
        self.clk.setStyleSheet("font-size: 24px; font-weight: 700;")
        lay.addWidget(self.clk)
        self.date_lbl = QLabel()
        self.date_lbl.setObjectName("t_small")
        lay.addWidget(self.date_lbl)
        lay.addSpacing(24)

        self.svc_pills = {}
        for n in ("router", "rag", "llm", "tts", "tool"):
            p = QLabel(n)
            p.setProperty("pill", True)
            lay.addWidget(p)
            self.svc_pills[n] = p
        lay.addStretch()

        self.rk_pill = QLabel("RK 离线")
        self.rk_pill.setProperty("pill", True)
        lay.addWidget(self.rk_pill)
        self.theme_btn = IconButton("moon", "切换昼/夜主题")
        self.theme_btn.clicked.connect(self._toggle_theme)
        lay.addWidget(self.theme_btn)
        return bar

    def _content(self):
        self.stack = QStackedWidget()
        self._page_ids = {}

        # home（宿主内置：磁贴网格 + RK 快览条）
        home = QWidget()
        hl = QVBoxLayout(home)
        hl.setContentsMargins(24, 16, 24, 16)
        hl.setSpacing(16)
        for p in self.plugins:
            if hasattr(p.mod, "create_home_strip"):
                try:
                    hl.addWidget(p.mod.create_home_strip(self.ctx))
                except Exception as e:  # noqa: BLE001
                    print(f"[host] home_strip {p.id} 失败: {e}", flush=True)
        grid_holder = QWidget()
        gh = QHBoxLayout(grid_holder)
        gh.setContentsMargins(0, 0, 0, 0)
        gh.setSpacing(16)
        self._tiles = {}
        for p in self.plugins:
            tile = Tile(p, self.ctx)
            tile.clicked.connect(self.navigate)
            self._tiles[p.id] = tile
            gh.addWidget(tile, p.tile_span)
            try:
                p.create_card(self.ctx)  # 插件自喂磁贴摘要
            except Exception as e:  # noqa: BLE001
                print(f"[host] 磁贴 {p.id} 喂养失败: {e}", flush=True)
        gh.addStretch(1)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(grid_holder)
        scroll.setFixedHeight(240)
        hl.addWidget(scroll)
        hl.addStretch(1)
        self.stack.addWidget(home)
        self._page_ids["home"] = self.stack.count() - 1

        # 插件页
        for p in self.plugins:
            if p.page:
                try:
                    self.stack.addWidget(p.create_page(self.ctx))
                    self._page_ids[p.id] = self.stack.count() - 1
                except Exception as e:  # noqa: BLE001 —— 单坏插件不拖死整窗
                    print(f"[host] 页面 {p.id} 构建失败: {e}", flush=True)
        return self.stack

    def _bottombar(self):
        bar = QFrame()
        bar.setFixedHeight(128)
        lay = QHBoxLayout(bar)
        lay.setContentsMargins(24, 8, 24, 8)
        lay.setSpacing(16)

        # dock（home + page=True 插件）
        self.dock_btns = {}
        for pid in ["home"] + [p.id for p in self.plugins
                               if p.page and p.id in self._page_ids]:
            b = IconButton(self._icon_of(pid), self._name_of(pid))
            b.setFixedSize(56, 56)
            b.clicked.connect(lambda _, i=pid: self.navigate(i))
            lay.addWidget(b)
            self.dock_btns[pid] = b
        lay.addStretch()

        self.voice = VoiceArea(self.theme)
        lay.addWidget(self.voice, 1)
        return bar

    def _icon_of(self, pid):
        if pid == "home":
            return "house"
        for p in self.plugins:
            if p.id == pid:
                return p.icon
        return "gear-six"

    def _name_of(self, pid):
        if pid == "home":
            return "主页"
        for p in self.plugins:
            if p.id == pid:
                return p.name
        return pid

    def _voice(self):
        bus = self.ctx.bus
        bus.asr_text.connect(self.voice.on_asr)
        bus.tts_text.connect(self.voice.on_tts)
        bus.play_end.connect(self.voice.on_play_end)
        bus.nav.connect(self._on_nav)

    def _sources(self):
        ctx_mod.start_sources(self.ctx)
        self.ctx.bus.rk_online.connect(self._on_rk_online)

    # ── 路由 / toast / 主题 / 语音导航 ───────────────────────────────
    def navigate(self, pid):
        idx = self._page_ids.get(pid)
        if idx is None:
            self.toast("该功能未安装", "warn")
            return
        self.stack.setCurrentIndex(idx)
        for i, b in self.dock_btns.items():
            b.setChecked(i == pid)
            b._refresh()

    def _on_nav(self, target):
        print(f"[voice] nav -> {target}", flush=True)
        self.navigate(target)
        self.voice._set("idle", "")

    def toast(self, text, level="info"):
        t = AlertToast(text, level, self)
        t.setParent(self.centralWidget())
        t.adjustSize()
        t.move((self.centralWidget().width() - t.width()) // 2,
               72 + len(self._toast_queue) * 64)
        t.show()
        t.show_toast()
        self._toast_queue.append(t)
        t.closed.connect(
            lambda x: self._toast_queue.remove(x) if x in self._toast_queue else None)

    def _toggle_theme(self):
        self.theme.toggle()
        self.theme_btn.setName("moon" if self.theme.name == "dark" else "sun")

    def _on_theme(self, T):
        icons.set_theme(T)
        self.theme_btn._refresh()
        for b in self.dock_btns.values():
            b._refresh()

    def _on_rk_online(self, online):
        if online:
            set_pill(self.rk_pill, "RK 在线", "ok")
        else:
            set_pill(self.rk_pill, "RK 离线", "danger")
            self.toast("行车记录仪离线：状态查询无应答", "danger")

    def _on_service_status(self, m):
        """6671 服务状态 pill（保留 v1 语义：闪 4s 回灰）。"""
        s = m.get("service", "?")
        st = m.get("status", "")
        lbl = self.svc_pills.get(s)
        if lbl is None:
            return
        set_pill(lbl, f"{s}: {st}"[:18], "ok")
        QTimer.singleShot(4000, lambda: set_pill(lbl, s, "idle"))

    def _tick(self):
        now = datetime.datetime.now()
        self.clk.setText(now.strftime("%H:%M"))
        self.date_lbl.setText(now.strftime("%m-%d %a"))

    def closeEvent(self, e):
        for t in self.ctx._threads:
            try:
                t.stop()
            except AttributeError:
                pass
        for t in self.ctx._threads:
            t.wait(2000)
        e.accept()


def main():
    vox_config.load()
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    win = Dashboard(app)
    win.show()

    # 无屏自测钩子：延时截图整窗（可指定页面）后自动退出
    snap = os.environ.get("VOX_DASH_SNAPSHOT")
    if snap:
        page = os.environ.get("VOX_DASH_SNAPSHOT_PAGE", "")
        delay = int(os.environ.get("VOX_DASH_SNAPSHOT_S", "8"))

        def _grab():
            if page:
                win.navigate(page)
                QTimer.singleShot(800, lambda: _save(win, snap))
            else:
                _save(win, snap)

        QTimer.singleShot(delay * 1000, _grab)
    sys.exit(app.exec_())


def _save(win, snap):
    win.grab().save(snap)
    print(f"[dash-snapshot] saved {snap}", flush=True)
    QApplication.instance().quit()


if __name__ == "__main__":
    main()
