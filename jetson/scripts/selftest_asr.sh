#!/usr/bin/env bash
# asr 自测（R14 后口径）：线上语音入口在 RK——voice_service（NPU 流式 ASR）+ tts_node（板端合成）。
# 本自测断言双板语音通路存活：RK 两进程在跑 + PULL :6711（PCM 注入）/ REP :6720（TTS 合成）监听。
# 识别正确性验证 = wav_push 注入真实语音 wav（RK /tmp/2.wav）或真麦，见 AGENTS.md §6 验证体系。
# 前置：start_voice.sh 已在 RK 拉起（start_all.sh ② 步自动做）。
set -euo pipefail

RK="${VOX_RK_BOARD:-lubancat}"

echo "[selftest:asr] 检查 RK 语音通路存活（$RK）"
ssh "$RK" 'set -e
  pgrep -f "[v]oice_service.py" >/dev/null || { echo "[selftest:asr] FAIL: voice_service 未运行（RK 上跑 ~/Desktop/VoxDrive/start_voice.sh）"; exit 1; }
  pgrep -x tts_node >/dev/null  || { echo "[selftest:asr] FAIL: tts_node 未运行"; exit 1; }
  ss -tln | grep -q ":6711 " || { echo "[selftest:asr] FAIL: PULL :6711 未监听"; exit 1; }
  ss -tln | grep -q ":6720 " || { echo "[selftest:asr] FAIL: REP :6720 未监听"; exit 1; }
  echo "  voice_service pid=$(pgrep -f "[v]oice_service.py" | head -1), tts_node pid=$(pgrep -x tts_node | head -1)"
'
echo "[selftest:asr] SELFTEST_ASR PASS（RK 语音入口存活：voice_service + tts_node，:6711/:6720 监听正常）"
