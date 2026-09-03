#!/usr/bin/env python3
"""RAG 服务 — 车辆手册知识库检索。

REP :port.rag 应答检索请求；状态 PUB → :port.intent_router_pub（全局状态总线）。
检索阈值/条数、模型路径、端口均读 voxdrive.conf。
"""
import json
import os
import pathlib
import sys

JETSON_ROOT = pathlib.Path(__file__).resolve().parents[2]  # services/rag/ → jetson/
sys.path.insert(0, str(JETSON_ROOT))
sys.path.insert(0, str(JETSON_ROOT / "services" / "rag" / "python"))

import zmq  # noqa: E402

from common import vox_config, vox_log  # noqa: E402
from vehicle_vector_search import VehicleVectorSearch  # noqa: E402


def main():
    vox_config.load()
    log = vox_log.setup("rag")
    vox_log.setup("vehicle_vector_search")  # 检索模块日志统一格式

    model_path = vox_config.get("model.rag_embedding")
    vector_db_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "python", "vector_db")
    threshold = vox_config.get_float("rag.threshold", 0.5)
    top_k = vox_config.get_int("rag.top_k", 3)

    log.info("加载嵌入模型(%s)与向量库...", model_path)
    searcher = VehicleVectorSearch(model_path, vector_db_path)
    searcher.load_model(model_path)
    stats = searcher.get_statistics()
    log.info("向量库就绪：%d docs, dim=%d", stats["total_documents"],
             stats["embedding_dimension"])

    ctx = zmq.Context()
    sock = ctx.socket(zmq.REP)
    sock.bind(vox_config.bind_endpoint("port.rag", "6667"))

    status_ctx = zmq.Context()
    status_sock = status_ctx.socket(zmq.PUB)
    status_sock.connect(vox_config.connect_endpoint("port.intent_router_pub", "6671"))

    log.info("listening %s", vox_config.bind_endpoint("port.rag", ""))

    while True:
        query = sock.recv_string()
        log.info("query: %s", query)

        results = searcher.search(query, top_k=top_k, threshold=threshold)
        passages = [r["text"] for r in results]
        text = "\n".join(passages) if passages else "抱歉，未找到相关信息。"

        sock.send_string(json.dumps({"found": bool(passages), "text": text},
                                    ensure_ascii=False))
        log.info("found=%s, %d chars", bool(passages), len(text))

        try:
            status_sock.send_string(json.dumps(
                {"service": "rag", "status": f"matched {len(passages)} docs"},
                ensure_ascii=False), zmq.NOBLOCK)
        except zmq.ZMQError:
            pass  # PUB 非阻塞发送，队列满丢弃状态消息可接受


if __name__ == "__main__":
    main()
