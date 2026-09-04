#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""手册文本 → 向量库（vehicle_embeddings.npy + vehicle_data.pkl + index_info.json）。

这是 RAG 向量库的正式构建脚本（此前 prj1 的 vector_db 为临时产物，无脚本）。

两种语料格式：
  1. Markdown 手册（如仓库自带 vehicle_manual_data.txt）
     '# ' 行 = 章节、'## ' 行 = 子章节，空行分段，段落打包 ≤ max_chars
  2. 页级 JSONL（extract_manual_pdf.py 产出，真实车主手册）
     每行 {"page": N, "text": ...}；--toc-pages 指定目录页范围时解析
     "标题.....页码" 条目建立 页→章节名 映射，元数据带真实章节名

用法（板上，用 conf 的嵌入模型）:
  /usr/bin/python3 build_vector_db.py \
      --corpus manual_pages.jsonl --toc-pages 4-8 \
      --model $HOME/Desktop/VoxDrive/models/embedding \
      --out vector_db
"""
import argparse
import datetime
import json
import os
import pickle
import re
import sys

import numpy as np

SENT_SPLIT = re.compile(r"(?<=[。！？；])")
TOC_LINE = re.compile(r".{2,30}\.{2,}\s*\d{1,3}\s*$")


def is_toc_chunk(chunk: str) -> bool:
    """目录型块：多数行是 '标题.....页码' 形态（各章开头的迷你目录页）。"""
    lines = [ln for ln in chunk.splitlines() if ln.strip()]
    if len(lines) < 3:
        return False
    hits = sum(1 for ln in lines if TOC_LINE.search(ln.strip()))
    return hits >= len(lines) * 0.6


def chunk_paragraphs(paras, max_chars, overlap_paras=0):
    """段落打包成 ≤ max_chars 的块；超长段落按句边界切。"""
    chunks = []
    buf = []

    def flush():
        if buf:
            chunks.append("\n".join(buf).strip())
            del buf[:]

    def add_long(p):
        sentences = SENT_SPLIT.split(p)
        cur = ""
        for s in sentences:
            if cur and len(cur) + len(s) > max_chars:
                chunks.append(cur.strip())
                cur = s
            else:
                cur += s
        if cur.strip():
            chunks.append(cur.strip())

    for p in paras:
        p = p.strip()
        if not p:
            continue
        if len(p) > max_chars:
            flush()
            add_long(p)
            continue
        if sum(len(x) for x in buf) + len(p) + len(buf) > max_chars:
            flush()
        buf.append(p)
    flush()
    return [c for c in chunks if len(c) >= 20]


def build_from_markdown(path, max_chars):
    section, subsection = "", ""
    cur_section, cur_sub = [], []
    texts, meta = [], []

    def flush():
        nonlocal cur_section, cur_sub
        if cur_section:
            for c in chunk_paragraphs(cur_section, max_chars):
                texts.append(c)
                meta.append({"section": section, "subsection": "", "page": 0})
        if cur_sub:
            for c in chunk_paragraphs(cur_sub, max_chars):
                texts.append(c)
                meta.append({"section": section, "subsection": subsection, "page": 0})
        cur_section, cur_sub = [], []

    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip()
            if line.startswith("## "):
                flush()
                subsection = line[3:].strip()
            elif line.startswith("# "):
                flush()
                section = line[2:].strip()
                subsection = ""
            elif line.startswith("### "):
                # 三级标题并入子章节文本（保持块上下文）
                cur_sub.append(line[4:].strip())
            else:
                if subsection:
                    cur_sub.append(line)
                else:
                    cur_section.append(line)
    flush()
    return texts, meta, {"format": "markdown", "sections": sorted({m["section"] for m in meta})}


def parse_toc_pages(jsonl_pages, toc_range):
    """从目录页解析 '标题.....页码' → {起始页: 章节标题}（解析失败返回空）。"""
    page_to_title = {}
    pat = re.compile(r"([\u4e00-\u9fffA-Za-z0-9\-/（）()·、\s]{2,30}?)\s*\.{2,}\s*(\d{1,3})\s*$")
    for rec in jsonl_pages:
        if rec["page"] not in toc_range:
            continue
        for ln in rec["text"].splitlines():
            m = pat.search(ln.strip())
            if m:
                title = re.sub(r"\s+", "", m.group(1))
                title = re.sub(r"^\d+(-\d+)?", "", title)  # 剥章节号前缀（如 4-1）
                # 过滤目录页自身的噪声（如纯数字章号行）
                if 2 <= len(title) <= 30 and not title.isdigit():
                    page_to_title[int(m.group(2))] = title
    return page_to_title


def build_from_jsonl(path, max_chars, toc_pages):
    pages = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                pages.append(json.loads(line))
    pages.sort(key=lambda r: r["page"])

    toc_range = set()
    if toc_pages:
        a, b = toc_pages
        toc_range = set(range(a, b + 1))
    page_to_title = parse_toc_pages(pages, toc_range) if toc_range else {}

    # 页 → 所属章节（目录条目按页序展开到下一章节起始页）
    # 页 → 所属章节（目录条目按页序展开到下一章节起始页；终点钳到语料最大页）
    max_page = max(r["page"] for r in pages)
    sorted_titles = sorted(page_to_title.items())
    title_by_page = {}
    for i, (start, title) in enumerate(sorted_titles):
        end = sorted_titles[i + 1][0] if i + 1 < len(sorted_titles) else max_page + 1
        for p in range(start, min(end, max_page + 1)):
            title_by_page[p] = title

    texts, meta = [], []
    dropped_toc = 0
    for rec in pages:
        if rec["page"] in toc_range:  # 主目录页本身不入库
            continue
        for c in chunk_paragraphs(rec["text"].splitlines(), max_chars):
            if is_toc_chunk(c):  # 章首迷你目录块：召回它们只会得到"标题+页码"
                dropped_toc += 1
                continue
            texts.append(c)
            meta.append({"section": title_by_page.get(rec["page"], f"第{rec['page']}页"),
                         "subsection": "",
                         "page": rec["page"]})
    info = {"format": "jsonl", "pages": len(pages), "toc_entries": len(page_to_title),
            "toc_chunks_dropped": dropped_toc}
    return texts, meta, info


def main():
    ap = argparse.ArgumentParser(description="手册文本 → RAG 向量库")
    ap.add_argument("--corpus", required=True, help=".txt(markdown) 或 .jsonl(extract_manual_pdf 产物)")
    ap.add_argument("--model", required=True, help="SentenceTransformer 模型路径")
    ap.add_argument("--out", default="vector_db")
    ap.add_argument("--max-chars", type=int, default=500)
    ap.add_argument("--toc-pages", default="", help="目录页范围 如 4-8（仅 jsonl）")
    args = ap.parse_args()

    if args.corpus.endswith(".jsonl"):
        toc = None
        if args.toc_pages:
            a, b = args.toc_pages.split("-")
            toc = (int(a), int(b))
        texts, meta, info = build_from_jsonl(args.corpus, args.max_chars, toc)
    else:
        texts, meta, info = build_from_markdown(args.corpus, args.max_chars)

    if not texts:
        print("错误：未切出任何文本块，请检查语料格式", file=sys.stderr)
        sys.exit(1)
    print(f"切块完成：{len(texts)} 块（max_chars={args.max_chars}）")

    from sentence_transformers import SentenceTransformer
    device = os.environ.get("RAG_EMBEDDING_DEVICE", "cpu")
    print(f"加载嵌入模型 {args.model} (device={device}) ...")
    model = SentenceTransformer(args.model, device=device)
    embeddings = model.encode(texts, batch_size=32, show_progress_bar=True)
    embeddings = np.asarray(embeddings, dtype=np.float32)

    os.makedirs(args.out, exist_ok=True)
    np.save(os.path.join(args.out, "vehicle_embeddings.npy"), embeddings)
    with open(os.path.join(args.out, "vehicle_data.pkl"), "wb") as f:
        pickle.dump({"texts": texts, "metadata": meta}, f)
    index_info = {
        "built_at": datetime.datetime.now().isoformat(timespec="seconds"),
        "model": os.path.basename(os.path.normpath(args.model)),
        "corpus": os.path.basename(args.corpus),
        "chunks": len(texts),
        "dim": int(embeddings.shape[1]),
        **info,
    }
    with open(os.path.join(args.out, "index_info.json"), "w", encoding="utf-8") as f:
        json.dump(index_info, f, ensure_ascii=False, indent=2)

    print(f"完成 → {args.out}/  chunks={len(texts)} dim={embeddings.shape[1]}")
    print(json.dumps(index_info, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
