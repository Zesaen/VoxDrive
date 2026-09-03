#!/usr/bin/env bash
# 准备 libavformat 头文件（C3/R3 MP4 封装用）——在 RK 板上执行。
#
# 为什么不解 apt 装 libavformat-dev：板上 ffmpeg 运行库是野火 BSP 的 rkmpp 补丁版
#（4.4.2+rkmpp20230327），官方 dev 包依赖对不上（会要求降级系统库）；厂商 PPA 的
# dev 包是 6.0.1 大版本也不匹配。因此只取官方 4.4.2 的头文件解包到项目前缀，
# 链接仍指向系统同名 .so.58/.56（同 4.4.2 源，rkmpp 补丁不改核心封装 ABI）。
# 注意：Ubuntu 22.04 multiarch 布局，头文件在 usr/include/aarch64-linux-gnu/ 下。
# 幂等：已存在则跳过。
set -euo pipefail

PREFIX="$HOME/Desktop/VoxDrive/third_party/ffmpeg-dev"
INC="$PREFIX/usr/include/aarch64-linux-gnu"
if [ -f "$INC/libavformat/avformat.h" ]; then
  echo "ffmpeg headers already at $INC"
  exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
apt-get download libavformat-dev:arm64 libavcodec-dev:arm64 libavutil-dev:arm64 \
  libswresample-dev:arm64 >/dev/null
mkdir -p "$PREFIX"
for f in *.deb; do dpkg -x "$f" "$PREFIX"; done

ls "$INC/libavformat/avformat.h" "$INC/libavcodec/avcodec.h" \
  "$INC/libavutil/avutil.h" >/dev/null
echo "headers ready at $INC"
