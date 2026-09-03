"""vox_log — VoxDrive 统一日志（与 C++ vox_log.h 同格式）。

格式：2026-09-03 21:45:12.345 [INFO ] [tool_bus] 消息内容

用法：
    from common import vox_log
    logger = vox_log.setup("rag")
    logger.info("检索完成 top1=%.3f", score)

级别：$VOX_LOG 或调用参数（debug/info/warn/error），默认 info。输出 stderr，
由启动脚本重定向 /tmp/<service>.log。
"""
import logging
import os
import sys
import time


class _MsFormatter(logging.Formatter):
    """时间戳精确到毫秒，格式与 C++ vox_log.h 完全一致（跨服务日志对账用）。"""

    def formatTime(self, record, datefmt=None):  # noqa: N802（logging 固定接口）
        ct = self.converter(record.created)
        t = time.strftime("%Y-%m-%d %H:%M:%S", ct)
        return f"{t}.{int(record.msecs):03d}"


_LEVELS = {"debug": logging.DEBUG, "info": logging.INFO,
           "warn": logging.WARNING, "error": logging.ERROR}


def setup(service: str, level: str | None = None) -> logging.Logger:
    """初始化并返回以服务名为名的 logger（重复调用幂等）。"""
    logger = logging.getLogger(service)
    if not logger.handlers:  # 幂等：仅首次挂 handler
        handler = logging.StreamHandler(sys.stderr)
        handler.setFormatter(
            _MsFormatter("%(asctime)s [%(levelname)-5s] [%(name)s] %(message)s"))
        logger.addHandler(handler)
        logger.propagate = False
    lv = level or os.environ.get("VOX_LOG", "info")
    logger.setLevel(_LEVELS.get(lv.lower(), logging.INFO))
    return logger
