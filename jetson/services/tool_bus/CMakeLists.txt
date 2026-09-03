cmake_minimum_required(VERSION 3.12)
project(tool_bus VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_search_module(ZMQ REQUIRED libzmq)
find_package(Threads REQUIRED)

# 板上布局：jetson/services/tool_bus 向上三级 = VoxDrive 根（zmq-comm-kit 与 jetson/ 同级）
set(VOX_ROOT ${CMAKE_SOURCE_DIR}/../../..)
set(ZMQ_COMM_KIT_DIR ${VOX_ROOT}/zmq-comm-kit)
set(VOX_COMMON_DIR ${CMAKE_SOURCE_DIR}/../../common)

include_directories(
    ${CMAKE_SOURCE_DIR}
    ${ZMQ_COMM_KIT_DIR}/include
    ${VOX_COMMON_DIR}
)

add_executable(tool_bus main.cpp)

# 链接绝对路径 + 写入 rpath，运行时无需 LD_LIBRARY_PATH
target_link_libraries(tool_bus
    ${ZMQ_COMM_KIT_DIR}/build/libzmq_component.so
    zmq
    Threads::Threads
)
set_target_properties(tool_bus PROPERTIES
    BUILD_RPATH ${ZMQ_COMM_KIT_DIR}/build
    INSTALL_RPATH ${ZMQ_COMM_KIT_DIR}/build
)
