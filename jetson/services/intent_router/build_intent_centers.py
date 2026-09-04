#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线构建语义路由意图中心文件（intent_centers.bin）。

用法（板上，用 conf 的嵌入模型，与 RAG 运行时编码一致）:
  /usr/bin/python3 build_intent_centers.py \
      --model $HOME/Desktop/VoxDrive/models/embedding \
      --output intent_centers.bin \
      --calibrate

产物格式（semantic_router.h load_from_file 对应）:
  [int32 num][int32 dim]
  每个意图: [int32 name_len][utf-8 name][int32 priority][float threshold][float*dim center]

--calibrate：用与构建样本不重叠的留出探针句实测各类命中相似度，
输出建议阈值（min 正确命中 - 0.03 余量）并写入 bin。
阈值是实测校准值——不用 INTENT_SAMPLES 里的手写初值。
"""
import argparse
import struct

import numpy as np
from sentence_transformers import SentenceTransformer

# ── 意图模板: 每类构建样本（中心向量用） ──────────────────────
INTENT_SAMPLES = {
    "EMERGENCY": {
        "priority": 0,    # 最高优先级 (0 = 不可被覆盖)
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

# ── 留出探针（不与构建样本重复）：校准阈值 + 验收正确率 ──────
INTENT_PROBES = {
    "EMERGENCY": [
        "刹车 warning 灯亮了还能开吗",
        "方向盘突然变得很沉怎么办",
        "水温报警了",
    ],
    "EXPLICIT_CMD": [
        "把空调打开",
        "帮我把车窗关上",
        "来点音乐",
    ],
    "FACTUAL": [
        "这车百公里耗几个油",
        "多久做一次首保",
        "雨刮器多久换一次",
        "玻璃水该加哪一种",
    ],
    "COMPLEX": [
        "起步的时候车身为什么会抖",
        "开空调以后动力变弱正常吗",
    ],
    "CREATIVE": [
        "路上太无聊了说点有趣的",
        "推荐个自驾游的路线",
        "讲个段子听听",
    ],
}

# 手写阈值仅作初值（无 --calibrate 时用）；校准后以实测值覆盖
INITIAL_THRESHOLDS = {"EMERGENCY": 0.35, "EXPLICIT_CMD": 0.5,
                      "FACTUAL": 0.45, "COMPLEX": 0.4, "CREATIVE": 0.4}


def build_centers(model):
    centers = {}
    for name, cfg in INTENT_SAMPLES.items():
        emb = model.encode(cfg["samples"])
        center = emb.mean(axis=0)
        norm = np.linalg.norm(center)
        if norm > 0:
            center /= norm
        centers[name] = center.astype(np.float32)
        print(f"  center {name}: {len(cfg['samples'])} samples")
    return centers


def calibrate(model, centers):
    """探针句逐类实测 top-1 命中相似度 → 每类建议阈值。"""
    thresholds = {}
    all_correct = 0
    total = 0
    print("\n=== 校准（留出探针） ===")
    for name in INTENT_SAMPLES:
        sims = {n: [] for n in INTENT_SAMPLES}
        for probe in INTENT_PROBES[name]:
            vec = model.encode([probe])[0]
            vec = vec / (np.linalg.norm(vec) + 1e-12)
            scored = sorted(((float(np.dot(vec, c)), n) for n, c in centers.items()),
                            reverse=True)
            top_sim, top_name = scored[0]
            sims[top_name].append(top_sim)
            correct = top_name == name
            all_correct += correct
            total += 1
            mark = "OK " if correct else "MISS"
            second = f" (次高 {scored[1][1]} {scored[1][0]:.3f})" if not correct else ""
            print(f"  [{mark}] {name} <- '{probe}' top1={top_name} {top_sim:.3f}{second}")
        correct_sims = sims[name]
        if correct_sims:
            thresholds[name] = max(0.35, round(min(correct_sims) - 0.03, 3))
        else:
            thresholds[name] = INITIAL_THRESHOLDS[name]
            print(f"  [WARN] {name} 无正确命中，保留初值 {thresholds[name]}")
    print(f"\n探针正确率: {all_correct}/{total}")
    print("建议阈值:", {k: thresholds[k] for k in INTENT_SAMPLES})
    return thresholds, all_correct, total


def save(centers, thresholds, output):
    dim = len(next(iter(centers.values())))
    with open(output, "wb") as f:
        f.write(struct.pack("<ii", len(centers), dim))
        for name, center in centers.items():
            cfg = INTENT_SAMPLES[name]
            nb = name.encode("utf-8")
            f.write(struct.pack("<i", len(nb)))
            f.write(nb)
            f.write(struct.pack("<if", cfg["priority"], thresholds[name]))
            f.write(center.tobytes())
    print(f"\nSaved {len(centers)} intents (dim={dim}) -> {output}")


def main():
    ap = argparse.ArgumentParser(description="构建语义路由意图中心")
    ap.add_argument("--model", required=True, help="SentenceTransformer 模型路径")
    ap.add_argument("--output", default="intent_centers.bin")
    ap.add_argument("--calibrate", action="store_true",
                    help="用留出探针实测阈值并写入（不指定则用手写初值）")
    args = ap.parse_args()

    print(f"Loading model: {args.model}")
    model = SentenceTransformer(args.model, device="cpu")

    print("Building intent centers...")
    centers = build_centers(model)

    if args.calibrate:
        thresholds, ok, total = calibrate(model, centers)
        if total and ok < total * 0.6:
            print("[WARN] 探针正确率 < 60%，建议先扩充样本再上线")
    else:
        thresholds = {n: INITIAL_THRESHOLDS[n] for n in INTENT_SAMPLES}

    save(centers, thresholds, args.output)


if __name__ == "__main__":
    main()
