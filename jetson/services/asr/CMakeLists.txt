cmake_minimum_required(VERSION 3.12)
project(sherpa_onnx_microphone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- paths -------------------------------------------------------------
# sherpa-onnx vendored 树（gitignore，板上一次性部署，见 models_manifest.md）
set(SHERPA_ONNX_DIR    ${CMAKE_SOURCE_DIR}/sherpa-onnx)
set(SHERPA_ONNX_LIBDIR ${SHERPA_ONNX_DIR}/build/lib)
# 板上布局：jetson/services/asr 向上三级 = VoxDrive 根（zmq-comm-kit 与 jetson/ 同级）
set(VOX_ROOT           ${CMAKE_SOURCE_DIR}/../../..)
set(ZMQ_COMM_KIT_DIR   ${VOX_ROOT}/zmq-comm-kit)
set(VOX_COMMON_DIR     ${CMAKE_SOURCE_DIR}/../../common)
# onnxruntime 板上安装前缀（可用 -DONNXRUNTIME_DIR=... 覆盖）
set(ONNXRUNTIME_DIR    /home/nvidia CACHE PATH "onnxruntime prefix (board)")

find_package(Threads REQUIRED)

# ---- include directories -----------------------------------------------
# _deps paths taken from sherpa-onnx's own build flags
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${SHERPA_ONNX_DIR}
    ${SHERPA_ONNX_DIR}/build/_deps/kaldi_decoder-src
    ${SHERPA_ONNX_DIR}/build/_deps/websocketpp-src
    ${SHERPA_ONNX_DIR}/build/_deps/asio-src/asio/include
    ${SHERPA_ONNX_DIR}/build/_deps/json-src/include
    ${SHERPA_ONNX_DIR}/build/_deps/hclust_cpp-src
    ${SHERPA_ONNX_DIR}/build/_deps/kaldi_native_fbank-src
    ${SHERPA_ONNX_DIR}/build/_deps/kissfft-src
    ${SHERPA_ONNX_DIR}/build/_deps/kaldifst-src
    ${SHERPA_ONNX_DIR}/build/_deps/openfst-src/src/include
    ${SHERPA_ONNX_DIR}/build/_deps/eigen-src
    ${SHERPA_ONNX_DIR}/build/_deps/simple-sentencepiece-src
    ${SHERPA_ONNX_DIR}/build/_deps/piper_phonemize-src/src
    ${SHERPA_ONNX_DIR}/build/_deps/espeak_ng-src/include
    ${SHERPA_ONNX_DIR}/build/_deps/piper_phonemize-src/src/include
    ${SHERPA_ONNX_DIR}/build/_deps/espeak_ng-src/src/include
    ${SHERPA_ONNX_DIR}/build/_deps/espeak_ng-src/src/ucd-tools/src/include
    ${SHERPA_ONNX_DIR}/build/_deps/onnxruntime-src/include
    ${ZMQ_COMM_KIT_DIR}/include
    ${VOX_COMMON_DIR}
)

# ---- executable --------------------------------------------------------
add_executable(sherpa-onnx-microphone
    sherpa-onnx-microphone.cc
    microphone.cc
)

# R14：ASR 推理下沉 RK NPU 后的 Jetson 侧采集件（仅 portaudio + zmq，无 sherpa 依赖）
add_executable(mic_stream mic_stream.cc)
target_include_directories(mic_stream PRIVATE
    ${ZMQ_COMM_KIT_DIR}/include
    ${VOX_COMMON_DIR}
)
target_link_libraries(mic_stream
    ${ZMQ_COMM_KIT_DIR}/build/libzmq_component.so
    portaudio
    zmq
    Threads::Threads
)
set_target_properties(mic_stream PROPERTIES
    BUILD_RPATH "${ZMQ_COMM_KIT_DIR}/build"
    INSTALL_RPATH "${ZMQ_COMM_KIT_DIR}/build"
)

# ---- link libraries ----------------------------------------------------
target_link_libraries(sherpa-onnx-microphone
    # sherpa-onnx core + its bundled deps
    ${SHERPA_ONNX_LIBDIR}/libsherpa-onnx-core.a
    ${SHERPA_ONNX_LIBDIR}/libsherpa-onnx-portaudio_static.a
    ${SHERPA_ONNX_LIBDIR}/libkaldi-decoder-core.a
    ${SHERPA_ONNX_LIBDIR}/libkaldi-native-fbank-core.a
    ${SHERPA_ONNX_LIBDIR}/libsherpa-onnx-kaldifst-core.a
    ${SHERPA_ONNX_LIBDIR}/libsherpa-onnx-fst.a
    ${SHERPA_ONNX_LIBDIR}/libsherpa-onnx-fstfar.a
    ${SHERPA_ONNX_LIBDIR}/libssentencepiece_core.a
    ${SHERPA_ONNX_LIBDIR}/libkissfft-float.a
    ${SHERPA_ONNX_LIBDIR}/libpiper_phonemize.a
    ${SHERPA_ONNX_LIBDIR}/libespeak-ng.a
    ${SHERPA_ONNX_LIBDIR}/libucd.a
    # zmq
    ${ZMQ_COMM_KIT_DIR}/build/libzmq_component.so
    # system libs
    ${ONNXRUNTIME_DIR}/lib/libonnxruntime.so
    portaudio
    zmq
    jack
    asound
    Threads::Threads
    dl
)

set_target_properties(sherpa-onnx-microphone PROPERTIES
    BUILD_RPATH "${ZMQ_COMM_KIT_DIR}/build;${SHERPA_ONNX_LIBDIR};${ONNXRUNTIME_DIR}/lib"
    INSTALL_RPATH "${ZMQ_COMM_KIT_DIR}/build;${SHERPA_ONNX_LIBDIR};${ONNXRUNTIME_DIR}/lib"
)
