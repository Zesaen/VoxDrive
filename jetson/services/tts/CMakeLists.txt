cmake_minimum_required(VERSION 3.12)
project(tts_server LANGUAGES CXX)

# 沿用 prj1 的 -O3 -fopenmp 裸 flag（vendored SummerTTS 的构建行为保持一致）
set(CMAKE_C_FLAGS " -O3 -fopenmp ${CMAKE_C_FLAGS} ")
set(CMAKE_CXX_FLAGS " -O3 -fopenmp ${CMAKE_CXX_FLAGS} ")
set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_search_module(ALSA REQUIRED alsa)
pkg_search_module(ZMQ REQUIRED libzmq)
find_package(Threads REQUIRED)

# 板上布局：jetson/services/tts 向上三级 = VoxDrive 根（zmq-comm-kit 与 jetson/ 同级）
set(VOX_ROOT ${CMAKE_SOURCE_DIR}/../../..)
set(ZMQ_COMM_KIT_DIR ${VOX_ROOT}/zmq-comm-kit)
set(VOX_COMMON_DIR ${CMAKE_SOURCE_DIR}/../../common)

include_directories(
    ${CMAKE_SOURCE_DIR}/tts_server/include
    ${ZMQ_COMM_KIT_DIR}/include
    ${VOX_COMMON_DIR}
    ${ALSA_INCLUDE_DIRS}
)

file(GLOB TTS_SERVER_SOURCES tts_server/src/*.cpp)

# vendored SummerTTS 引擎：文本正则化（glog/gflags/openfst 子集）+ 引擎主体
set(TN_SOURCES
    ./src/tn/glog/src/demangle.cc
    ./src/tn/glog/src/logging.cc
    ./src/tn/glog/src/raw_logging.cc
    ./src/tn/glog/src/symbolize.cc
    ./src/tn/glog/src/utilities.cc
    ./src/tn/glog/src/vlog_is_on.cc
    ./src/tn/glog/src/signalhandler.cc
    ./src/tn/gflags/src/gflags.cc
    ./src/tn/gflags/src/gflags_reporting.cc
    ./src/tn/gflags/src/gflags_completions.cc
    ./src/tn/openfst/src/lib/compat.cc
    ./src/tn/openfst/src/lib/flags.cc
    ./src/tn/openfst/src/lib/fst.cc
    ./src/tn/openfst/src/lib/fst-types.cc
    ./src/tn/openfst/src/lib/mapped-file.cc
    ./src/tn/openfst/src/lib/properties.cc
    ./src/tn/openfst/src/lib/symbol-table.cc
    ./src/tn/openfst/src/lib/symbol-table-ops.cc
    ./src/tn/openfst/src/lib/util.cc
    ./src/tn/openfst/src/lib/weight.cc
    ./src/tn/processor.cc
    ./src/tn/token_parser.cc
    ./src/tn/utf8_string.cc
    ./src/engipa/EnglishText2Id.cpp
    ./src/engipa/InitIPASymbols.cpp
    ./src/engipa/alphabet.cpp
    ./src/engipa/ipa.cpp
    ./src/hz2py/hanzi2phoneid.cpp
    ./src/hz2py/Hanz2Piny.cpp
    ./src/hz2py/pinyinmap.cpp
    ./src/nn_op/nn_conv1d.cpp
    ./src/nn_op/nn_softmax.cpp
    ./src/nn_op/nn_layer_norm.cpp
    ./src/nn_op/nn_relu.cpp
    ./src/nn_op/nn_gelu.cpp
    ./src/nn_op/nn_tanh.cpp
    ./src/nn_op/nn_flip.cpp
    ./src/nn_op/nn_cumsum.cpp
    ./src/nn_op/nn_softplus.cpp
    ./src/nn_op/nn_clamp_min.cpp
    ./src/nn_op/nn_sigmoid.cpp
    ./src/nn_op/nn_conv1d_transposed.cpp
    ./src/nn_op/nn_leaky_relu.cpp
    ./src/platform/tts_file_io.cpp
    ./src/platform/tts_logger.cpp
    ./src/utils/utils.cpp
    ./src/modules/iStft.cpp
    ./src/modules/hann.cpp
    ./src/modules/attention_encoder.cpp
    ./src/modules/multi_head_attention.cpp
    ./src/modules/ffn.cpp
    ./src/modules/ConvFlow.cpp
    ./src/modules/DDSConv.cpp
    ./src/modules/ElementwiseAffine.cpp
    ./src/modules/random_gen.cpp
    ./src/modules/ResidualCouplingLayer.cpp
    ./src/modules/ResBlock1.cpp
    ./src/modules/WN.cpp
    ./src/modules/pqmf.cpp
    ./src/models/TextEncoder.cpp
    ./src/models/StochasticDurationPredictor.cpp
    ./src/models/FixDurationPredictor.cpp
    ./src/models/DurationPredictor_base.cpp
    ./src/models/ResidualCouplingBlock.cpp
    ./src/models/Generator_base.cpp
    ./src/models/Generator_hifigan.cpp
    ./src/models/Generator_MS.cpp
    ./src/models/Generator_Istft.cpp
    ./src/models/Generator_MBB.cpp
    ./src/models/SynthesizerTrn.cpp
)

add_executable(tts_server ${TTS_SERVER_SOURCES} ${TN_SOURCES})

target_include_directories(tts_server PUBLIC
    ${CMAKE_SOURCE_DIR}/eigen-3.4.0   # 板上一次性部署（gitignore，见 models_manifest.md）
    ./src/tn/header
    ./include
    ./src/header
    ./tts_server/include)

# 链接绝对路径 + rpath，运行时无需 LD_LIBRARY_PATH
target_link_libraries(tts_server
    ${ZMQ_COMM_KIT_DIR}/build/libzmq_component.so
    ${ZMQ_LIBRARIES}
    portaudio
    ${ALSA_LIBRARIES}
    Threads::Threads)
set_target_properties(tts_server PROPERTIES
    BUILD_RPATH ${ZMQ_COMM_KIT_DIR}/build
    INSTALL_RPATH ${ZMQ_COMM_KIT_DIR}/build)
