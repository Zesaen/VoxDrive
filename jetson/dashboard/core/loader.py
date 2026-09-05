"""loader — 插件扫描/校验/注册（设计规范 5.2）。

plugins/<id>.py 模块级 MANIFEST + create_card(ctx) [+ create_page(ctx)]，
宿主反射加载。单个坏插件跳过并告警，不拖死整窗。
"""

import importlib.util
import pathlib
import sys

_PLUGINS_DIR = pathlib.Path(__file__).resolve().parents[1] / "plugins"

_REQUIRED = ("id", "name", "icon", "order")


class Plugin:
    def __init__(self, mod):
        self.mod = mod
        m = getattr(mod, "MANIFEST", {})
        self.id = m.get("id", "")
        self.name = m.get("name", "")
        self.icon = m.get("icon", "")
        self.order = m.get("order", 99)
        self.tile_span = m.get("tile_span", 1)
        self.page = bool(m.get("page", False))
        self.nav_words = list(m.get("nav_words", []))
        self._card = None
        self._page = None

    def create_card(self, ctx):
        if self._card is None:
            self._card = self.mod.create_card(ctx)
        return self._card

    def create_page(self, ctx):
        if self._page is None:
            self._page = self.mod.create_page(ctx)
        return self._page


def scan():
    """扫描 plugins/*.py → 校验 → 按 order 排序。返回 Plugin 列表。"""
    out = []
    if _PLUGINS_DIR.name == "plugins" and not _PLUGINS_DIR.is_dir():
        return out
    if str(_PLUGINS_DIR.parent) not in sys.path:
        sys.path.insert(0, str(_PLUGINS_DIR.parent))
    for f in sorted(_PLUGINS_DIR.glob("*.py")):
        if f.name.startswith("_"):
            continue
        try:
            spec = importlib.util.spec_from_file_location(
                f"vox_plugins.{f.stem}", f)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            p = Plugin(mod)
            missing = [k for k in _REQUIRED if not getattr(p, k)]
            if missing:
                print(f"[loader] {f.name}: MANIFEST 缺 {missing}，跳过", flush=True)
                continue
            if not hasattr(mod, "create_card"):
                print(f"[loader] {f.name}: 缺 create_card，跳过", flush=True)
                continue
            if p.page and not hasattr(mod, "create_page"):
                print(f"[loader] {f.name}: page=True 但缺 create_page，跳过", flush=True)
                continue
            out.append(p)
        except Exception as e:  # noqa: BLE001 —— 单坏插件不拖死整窗
            print(f"[loader] 插件 {f.name} 加载失败: {e}", flush=True)
    out.sort(key=lambda p: p.order)
    return out
