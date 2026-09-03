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

# ---- 测试程序 ----
add_executable(test_capture apps/test_capture.cpp)
target_link_libraries(test_capture vox_capture)
