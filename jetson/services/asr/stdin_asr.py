#!/usr/bin/env python3
"""键盘输入替代 ASR 的联调工具：stdin 文本 → Intent Router，可选 TTS 阻塞门同步。

端口读 voxdrive.conf（port.intent_router / port.tts_block）。
"""

import pathlib
import sys

JETSON_ROOT = pathlib.Path(__file__).resolve().parents[2]  # services/asr/ → jetson/
sys.path.insert(0, str(JETSON_ROOT))

import zmq  # noqa: E402

from common import vox_config  # noqa: E402


def main():
    vox_config.load()
    ctx = zmq.Context()

    router = ctx.socket(zmq.REQ)
    router.connect(vox_config.connect_endpoint("port.intent_router", "6666"))
    block_ep = vox_config.connect_endpoint("port.tts_block", "6677")

    print(f"[stdin-asr] connected to {vox_config.connect_endpoint('port.intent_router', '')}")
    print("[stdin-asr] type text and press enter, ctrl+c to quit\n")

    while True:
        try:
            text = input()
        except (EOFError, KeyboardInterrupt):
            print("\n[stdin-asr] exiting...")
            break

        text = text.strip()
        if not text:
            continue

        router.send_string(text)
        reply = router.recv_string()
        print(f"[router] {reply}")

        # TTS 阻塞门：每次新 socket（TTS 未运行时不卡 REQ 状态机）
        try:
            tts = ctx.socket(zmq.REQ)
            tts.setsockopt(zmq.RCVTIMEO, 1000)
            tts.setsockopt(zmq.LINGER, 0)
            tts.connect(block_ep)
            tts.send_string("block")
            tts.recv_string()
            tts.close()
        except zmq.Again:
            pass  # TTS not running

        sys.stdout.flush()


if __name__ == "__main__":
    main()
