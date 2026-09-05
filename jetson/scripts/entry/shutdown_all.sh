#!/usr/bin/env bash
# 顶层入口：优雅停服务→关 RK→关本机（转调 jetson/scripts/shutdown_all.sh）
exec "$(dirname "$0")/jetson/scripts/shutdown_all.sh" "$@"
