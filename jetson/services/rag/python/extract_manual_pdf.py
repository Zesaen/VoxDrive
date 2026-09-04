#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""车主手册 PDF → 页级 JSONL 文本（build_vector_db.py 的输入）。

用法:
    python extract_manual_pdf.py --pdf 车主手册.pdf --output manual_pages.jsonl

产物每行一个 JSON: {"page": 页码1起, "text": 该页清洗后文本}
仅适用于文本型 PDF（扫描件需先 OCR）。页眉/页脚中的纯页码行会被剔除。
"""
import argparse
import json
import re


def clean_page(text: str) -> str:
    # pypdf 常见问题：中文字间多余空格（提取伪影）、页脚孤立页码行
    lines = []
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        if re.fullmatch(r"-?\s*\d+\s*-?", ln):  # 纯页码行
            continue
        lines.append(ln)
    # 中文之间的单个空格是提取伪影（英文词间空格保留）
    out = "\n".join(lines)
    out = re.sub(r"(?<=[\u4e00-\u9fff，。、；：！？（）])\s+(?=[\u4e00-\u9fff，。、；：！？（）])", "", out)
    return out.strip()


def main():
    ap = argparse.ArgumentParser(description="车主手册 PDF → 页级 JSONL")
    ap.add_argument("--pdf", required=True)
    ap.add_argument("--output", required=True)
    ap.add_argument("--min-chars", type=int, default=30,
                    help="少于该字符数的页（封面/版权/空白）丢弃")
    args = ap.parse_args()

    from pypdf import PdfReader  # 延迟导入，环境未装时仅本脚本受影响
    reader = PdfReader(args.pdf)

    kept, dropped = 0, 0
    with open(args.output, "w", encoding="utf-8") as f:
        for i, page in enumerate(reader.pages, start=1):
            text = clean_page(page.extract_text() or "")
            if len(text) < args.min_chars:
                dropped += 1
                continue
            f.write(json.dumps({"page": i, "text": text}, ensure_ascii=False) + "\n")
            kept += 1

    total = len(reader.pages)
    print(f"{args.pdf}: {total} 页 → 保留 {kept} 页 / 丢弃 {dropped} 页（过短） → {args.output}")


if __name__ == "__main__":
    main()
