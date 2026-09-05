#!/usr/bin/env bash
# 顶层入口：四用例语音链路回归（转调 jetson/scripts/run_regression.sh）
exec "$(dirname "$0")/jetson/scripts/run_regression.sh" "$@"
