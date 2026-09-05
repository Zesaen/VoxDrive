#!/usr/bin/env bash
# 自动求解 RKNN 数据集行序：失败信息给出 (文件, 期望形状)，交换后重试，直至 build 通过。
# 用法: bash solve_order.sh   （在 ~/rknn_conv 下）
set -u
cd ~/rknn_conv
ORDER_TXT=calib_encoder_npy.txt
LOG=/tmp/solve_order.log

for iter in $(seq 1 40); do
  PYTHONPATH=~/rknn_conv/pylib python3 convert_asr.py encoder models/encoder_sim.onnx > $LOG 2>&1
  if grep -q "\[encoder\] OK" $LOG; then
    echo "SOLVED at iter $iter"; break
  fi
  MSG=$(grep -oE "The input\('[^']+'\) shape \(([0-9, ]+)\) is wrong, expect 'nchw' like \(([0-9, ]+)\)!" $LOG | head -1)
  if [ -z "$MSG" ]; then
    echo "UNKNOWN ERROR:"; grep -E "Error|error" $LOG | tail -3; break
  fi
  FILE=$(echo "$MSG" | sed "s/The input('\([^']*\)').*/\1/")
  EXP=$(echo "$MSG" | sed "s/.*expect 'nchw' like (\([0-9, ]*\)).*/\1/" | tr -d ' ')
  BASE=$(dirname $FILE)
  CAND=""
  for f in $BASE/*.npy; do
    SHP=$(python3 -c "import numpy as np; print(','.join(map(str, np.load('$f').shape)))")
    if [ "$SHP" = "$EXP" ]; then CAND=$f; break; fi
  done
  if [ -z "$CAND" ]; then echo "no candidate for $EXP"; break; fi
  python3 - <<PY
f1, f2 = "$FILE", "$CAND"
lines = open("$ORDER_TXT").read().splitlines()
i = lines[0].split().index(f1)
j = lines[0].split().index(f2)
for r in range(len(lines)):
    parts = lines[r].split()
    parts[i], parts[j] = parts[j], parts[i]
    lines[r] = " ".join(parts)
open("$ORDER_TXT", "w").write("\n".join(lines) + "\n")
print(f"swapped pos {i}<->{j}")
PY
done
grep -E "\[encoder\]" $LOG | tail -2
