"""vox_config — VoxDrive 统一配置读取（与 C++ vox_config.h 同一配置文件）。

查找顺序：显式路径 → $VOX_CONF → 本包所在 jetson/config/voxdrive.conf。
值支持 $HOME 展开。服务启动时调用 load() 一次，之后 get/get_int 取值。
"""
import os
import pathlib

_STORE: dict = {}

_CONF_NAME = "voxdrive.conf"


def _trim(s: str) -> str:
    return s.strip(" \t\r\n")


def _expand_home(v: str) -> str:
    if v.startswith("$HOME"):
        return os.environ.get("HOME", "") + v[len("$HOME"):]
    return v


def _candidates(path: str | None) -> list:
    paths = []
    if path:
        paths.append(path)
    env = os.environ.get("VOX_CONF")
    if env:
        paths.append(env)
    # 本文件位于 jetson/common/，配置固定在 jetson/config/
    paths.append(str(pathlib.Path(__file__).resolve().parents[1] / "config" / _CONF_NAME))
    paths.append(_CONF_NAME)  # 当前目录兜底
    return paths


def load(path: str | None = None) -> bool:
    """解析配置；成功返回 True。"""
    global _STORE
    for p in _candidates(path):
        try:
            with open(p, encoding="utf-8") as f:
                parsed = {}
                for line in f:
                    line = line.split("#", 1)[0]
                    line = _trim(line)
                    if not line or "=" not in line:
                        continue
                    key, _, val = line.partition("=")
                    parsed[_trim(key)] = _expand_home(_trim(val))
                _STORE = parsed
                print(f"vox_config: loaded {len(parsed)} keys from {p}")
                return True
        except OSError:
            continue
    print("vox_config: no voxdrive.conf found (VOX_CONF unset)")
    return False


def has(key: str) -> bool:
    return key in _STORE


def get(key: str, default: str = "") -> str:
    return _STORE.get(key, default)


def get_int(key: str, default: int = 0) -> int:
    v = _STORE.get(key, "")
    try:
        return int(v)
    except ValueError:
        return default


def get_float(key: str, default: float = 0.0) -> float:
    v = _STORE.get(key, "")
    try:
        return float(v)
    except ValueError:
        return default


def bind_endpoint(port_key: str, default_port: str) -> str:
    """服务端 bind 端点：tcp://*:6669"""
    return f"tcp://{get('bind.host', '*')}:{get(port_key, default_port)}"


def connect_endpoint(port_key: str, default_port: str, host: str = "localhost") -> str:
    """客户端 connect 端点：tcp://localhost:6669"""
    return f"tcp://{host}:{get(port_key, default_port)}"
