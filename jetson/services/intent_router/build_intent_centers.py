#!/usr/bin/env python3
"""
离线构建意图中心向量文件

用法:
    python build_intent_centers.py \
        --model ../automotive_edge_rag/models \
        --output intent_centers.bin \
        --dim 768

产物 intent_centers.bin 格式:
    [int32] num_intents
    对每个 intent:
        [int32] name_len
        [char*] name
        [int32] priority
        [float] threshold
        [float*768] center vector
"""

import struct
import argparse
import numpy as np
from sentence_transformers import SentenceTransformer


# ── 意图模板: 每个意图 5-10 句示例 ──────────────────────────
INTENT_SAMPLES = {
    "EMERGENCY": {
        "priority": 0,    # 最高优先级 (0 = 不可被覆盖)
        "threshold": 0.3, # 低阈值 (宁误报不可漏报)
        "samples": [
            "发动机故障灯亮了怎么办",
            "刹车失灵了",
            "机油压力警告怎么处理",
            "车辆起火怎么逃生",
            "ABS故障灯亮了能继续开吗",
            "转向突然失控了",
            "制动踏板踩不下去了",
            "安全气囊警告怎么办",
            "发动机温度过高",
            "电池故障灯亮了",
        ],
    },
    "EXPLICIT_CMD": {
        "priority": 1,
        "threshold": 0.5,
        "samples": [
            "打开空调",
            "关闭车窗",
            "调到二十二度",
            "导航到最近的加油站",
            "播放音乐",
            "打开座椅加热",
            "接听电话",
            "拍照",
            "打开天窗",
            "切换到内循环",
            "音量调大一点",
            "静音",
            "下一首",
            "打开行车记录仪",
            "调节后视镜",
        ],
    },
    "FACTUAL": {
        "priority": 2,
        "threshold": 0.4,
        "samples": [
            "保养周期是多少公里",
            "轮胎气压应该打到多少",
            "油耗是多少",
            "发动机功率多大",
            "保修期限是多久",
            "机油多久换一次",
            "变速箱油多久更换",
            "这车用什么标号的汽油",
            "制动液多久换一次",
            "电池保修多少年",
            "冷却液加哪种",
            "火花塞多久换",
        ],
    },
    "COMPLEX": {
        "priority": 3,
        "threshold": 0.4,
        "samples": [
            "发动机有异响可能是什么原因怎么排查",
            "油耗突然增加了不少可能是什么问题",
            "空调不制冷和暖风不热是同一个故障吗",
            "制动时有异响应该检查哪些部件",
            "方向跑偏要做四轮定位还是动平衡",
            "冷车启动困难热车正常是什么故障",
            "发电机皮带异响需要立即更换吗",
            "变速箱顿挫是什么原因怎么解决",
        ],
    },
    "CREATIVE": {
        "priority": 4,
        "threshold": 0.4,
        "samples": [
            "推荐附近好玩的景点",
            "讲个笑话",
            "今天天气怎么样",
            "附近有什么好吃的餐厅",
            "推荐一首开车听的歌",
            "去机场怎么走最快",
            "周边有什么适合亲子的地方",
            "给我讲个故事",
            "有什么科普知识分享一下",
            "附近有加油站吗",
        ],
    },
}


def build_centers(model: SentenceTransformer, dim: int) -> dict:
    """为每个意图计算中心向量 (所有示例句编码后的均值)"""
    centers = {}
    for intent_name, cfg in INTENT_SAMPLES.items():
        embeddings = model.encode(cfg["samples"])
        center = embeddings.mean(axis=0)
        # L2 归一化
        norm = np.linalg.norm(center)
        if norm > 0:
            center /= norm
        centers[intent_name] = {
            "center":    center.astype(np.float32),
            "priority":  cfg["priority"],
            "threshold": cfg["threshold"],
        }
        print(f"  {intent_name}: {len(cfg['samples'])} samples, "
              f"center dim={center.shape}, norm={np.linalg.norm(center):.4f}")
    return centers


def save_centers(centers: dict, output_path: str):
    """保存为 C++ 可读取的二进制格式"""
    with open(output_path, "wb") as f:
        f.write(struct.pack("<i", len(centers)))
        for name, data in centers.items():
            name_bytes = name.encode("utf-8")
            f.write(struct.pack("<i", len(name_bytes)))
            f.write(name_bytes)
            f.write(struct.pack("<i", data["priority"]))
            f.write(struct.pack("<f", data["threshold"]))
            f.write(data["center"].tobytes())
    print(f"\nSaved {len(centers)} intent centers to: {output_path}")


def main():
    parser = argparse.ArgumentParser(description="构建意图中心向量")
    parser.add_argument("--model", required=True, help="SentenceTransformer 模型路径")
    parser.add_argument("--output", default="intent_centers.bin")
    parser.add_argument("--dim", type=int, default=768)
    args = parser.parse_args()

    print(f"Loading model: {args.model}")
    model = SentenceTransformer(args.model)

    print("\nBuilding intent centers...")
    centers = build_centers(model, args.dim)

    save_centers(centers, args.output)

    # 打印统计
    print("\n=== Intent Center Statistics ===")
    for name in sorted(centers, key=lambda n: centers[n]["priority"]):
        c = centers[name]
        print(f"  {name:20s}  pri={c['priority']}  thr={c['threshold']}"
              f"  range=[{c['center'].min():.4f}, {c['center'].max():.4f}]")


if __name__ == "__main__":
    main()
