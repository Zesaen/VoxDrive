#!/usr/bin/env bash
# 顶层入口：双板一键启动（转调 jetson/scripts/start_all.sh，由 sync_to_board.sh 自动部署到 ~/Desktop/VoxDrive/）
exec "$(dirname "$0")/jetson/scripts/start_all.sh" "$@"
