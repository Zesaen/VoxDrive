# VoxDrive RK 节点（行车记录媒体节点）构建
# 板端编译定稿：g++ 11.4 / cmake 3.22，MPP/RGA 头文件随 librockchip-mpp-dev / librga-dev 预装。
# 目录约定：板上仓库根 = ~/Desktop/VoxDrive（与 Jetson 约定一致，目录结构与仓库镜像），
# 公共头文件物理位于 jetson/common/（收编期先建在 Jetson 侧；common 为节点无关代码，
# Jetson 重连后可整体提升为顶层 common/）。
cmake_minimum_required(VERSION 3.10)
project(voxdrive_rk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

set(VOX_ROOT ${CMAKE_SOURCE_DIR}/..)
set(VOX_COMMON ${VOX_ROOT}/jetson/common)

# ---- 采集层（C1/R1）----
add_library(vox_capture capture/v4l2_capture.cpp)
target_include_directories(vox_capture
  PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/capture ${VOX_COMMON})

# ---- 编码层（C2/R2）：MPP 头文件随 librockchip-mpp-dev 装在 /usr/include/rockchip ----
add_library(vox_encode encode/mpp_encoder.cpp)
target_include_directories(vox_encode
  PUBLIC ${CMAKE_SOURCE_DIR}/encode ${CMAKE_SOURCE_DIR}/include ${VOX_COMMON}
         /usr/include/rockchip)
target_link_libraries(vox_encode PUBLIC rockchip_mpp)

# ---- 存储层（C3/R3）：MP4 封装走 libavformat ----
# 头文件：官方 4.4.2 dev 包解包到项目前缀（rk/scripts/setup_ffmpeg_headers.sh，不动系统包，
# 因板上运行库是野火 rkmpp 补丁版、官方 dev 依赖对不上）；链接直接指系统同名 .so（同源 ABI 兼容）。
set(FFMPEG_DEV "$ENV{HOME}/Desktop/VoxDrive/third_party/ffmpeg-dev")
set(FFMPEG_INC "${FFMPEG_DEV}/usr/include/aarch64-linux-gnu")
if(NOT EXISTS "${FFMPEG_INC}/libavformat/avformat.h")
  message(FATAL_ERROR "缺少 ffmpeg 头文件：先在板上运行 rk/scripts/setup_ffmpeg_headers.sh")
endif()
add_library(vox_storage storage/mp4_segment_sink.cpp)
target_include_directories(vox_storage
  PUBLIC ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/storage ${VOX_COMMON}
  PRIVATE ${FFMPEG_INC})
target_link_libraries(vox_storage PUBLIC
  /usr/lib/aarch64-linux-gnu/libavformat.so.58
  /usr/lib/aarch64-linux-gnu/libavcodec.so.58
  /usr/lib/aarch64-linux-gnu/libavutil.so.56)

# ---- 测试程序 ----
add_executable(test_capture apps/test_capture.cpp)
target_link_libraries(test_capture vox_capture)

add_executable(test_encode apps/test_encode.cpp)
target_link_libraries(test_encode vox_capture vox_encode)

add_executable(test_record apps/test_record.cpp)
target_link_libraries(test_record vox_capture vox_encode vox_storage)
