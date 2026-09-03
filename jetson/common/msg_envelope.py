"""msg_envelope — 跨板 ZMQ 消息信封（与 C++ msg_envelope.h 同一协议）。

统一信封：{"version":1, "type":"...", "timestamp_ms":..., "source":"...", "payload":{...}}
新增数据类型（GPS/IMU/检测事件/录像状态…）只需扩展 type，协议不变。
"""
import time

VERSION = 1

TYPE_STATUS = "status"        # 状态查询应答（录像/存储/水位）
TYPE_EVENT = "event"          # 异常事件上行（断流/水位告警/锁录）
TYPE_SNAPSHOT = "snapshot"    # 抓拍结果
# 预留：TYPE_DETECTION（RKNN 检测事件，E2）、TYPE_GPS / TYPE_IMU（无硬件事不实现）


def now_ms() -> int:
    return int(time.time() * 1000)


def make(type_: str, source: str, payload: dict) -> dict:
    return {
        "version": VERSION,
        "type": type_,
        "timestamp_ms": now_ms(),
        "source": source,
        "payload": payload,
    }


def valid(env: dict) -> bool:
    return (
        isinstance(env, dict)
        and env.get("version") == VERSION
        and "type" in env
        and "timestamp_ms" in env
        and "payload" in env
    )
