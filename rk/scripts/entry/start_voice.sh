#!/usr/bin/env bash
# 板根顶层入口：~/Desktop/VoxDrive/start_voice.sh（sync_to_rk.sh 自动部署）
exec "$(dirname "$0")/rk/scripts/start_voice.sh" "$@"
