#!/usr/bin/env bash
# 顶层入口：停止全栈服务（转调 jetson/scripts/stop_all.sh）
exec "$(dirname "$0")/jetson/scripts/stop_all.sh" "$@"
