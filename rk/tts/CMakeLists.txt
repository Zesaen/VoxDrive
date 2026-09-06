cmake_minimum_required(VERSION 3.12)
project(tts_node LANGUAGES CXX)

# 与 Jetson tts_server 同款 -O3 -fopenmp（vendored SummerTTS 构建行为一致）
set(CMAKE_C_FLAGS " -O3 -fopenmp ${CMAKE_C_FLAGS} ")
set(CMAKE_CXX_FLAGS " -O3 -fopenmp ${CMAKE_CXX_FLAGS} ")
set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)

# 引擎源码树在 jetson/services/tts（sync_to_rk 一并送板；eigen 为一次性资产 scp 部署）
set(TTS_ROOT ${CMAKE_SOURCE_DIR}/../../jetson/services/tts)
set(VOX_COMMON_DIR ${CMAKE_SOURCE_DIR}/../../jetson/common)

set(ENGINE_SOURCES
    ${TTS_ROOT}/src/tn/glog/src/demangle.cc
    ${TTS_ROOT}/src/tn/glog/src/logging.cc
    ${TTS_ROOT}/src/tn/glog/src/raw_logging.cc
    ${TTS_ROOT}/src/tn/glog/src/symbolize.cc
    ${TTS_ROOT}/src/tn/glog/src/utilities.cc
    ${TTS_ROOT}/src/tn/glog/src/vlog_is_on.cc
    ${TTS_ROOT}/src/tn/gflags/src/gflags.cc
    ${TTS_ROOT}/src/tn/gflags/src/gflags_reporting.cc
    ${TTS_ROOT}/src/tn/gflags/src/gflags_completions.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/compat.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/flags.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/fst.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/fst-types.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/mapped-file.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/properties.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/symbol-table.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/symbol-table-ops.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/util.cc
    ${TTS_ROOT}/src/tn/openfst/src/lib/weight.cc
    ${TTS_ROOT}/src/tn/processor.cc
    ${TTS_ROOT}/src/tn/token_parser.cc
    ${TTS_ROOT}/src/tn/utf8_string.cc
    ${TTS_ROOT}/src/engipa/EnglishText2Id.cpp
    ${TTS_ROOT}/src/engipa/InitIPASymbols.cpp
    ${TTS_ROOT}/src/engipa/alphabet.cpp
    ${TTS_ROOT}/src/engipa/ipa.cpp
    ${TTS_ROOT}/src/hz2py/hanzi2phoneid.cpp
    ${TTS_ROOT}/src/hz2py/Hanz2Piny.cpp
    ${TTS_ROOT}/src/hz2py/pinyinmap.cpp
    ${TTS_ROOT}/src/nn_op/nn_conv1d.cpp
    ${TTS_ROOT}/src/nn_op/nn_softmax.cpp
    ${TTS_ROOT}/src/nn_op/nn_layer_norm.cpp
    ${TTS_ROOT}/src/nn_op/nn_relu.cpp
    ${TTS_ROOT}/src/nn_op/nn_gelu.cpp
    ${TTS_ROOT}/src/nn_op/nn_tanh.cpp
    ${TTS_ROOT}/src/nn_op/nn_flip.cpp
    ${TTS_ROOT}/src/nn_op/nn_cumsum.cpp
    ${TTS_ROOT}/src/nn_op/nn_softplus.cpp
    ${TTS_ROOT}/src/nn_op/nn_clamp_min.cpp
    ${TTS_ROOT}/src/nn_op/nn_sigmoid.cpp
    ${TTS_ROOT}/src/nn_op/nn_conv1d_transposed.cpp
    ${TTS_ROOT}/src/nn_op/nn_leaky_relu.cpp
    ${TTS_ROOT}/src/platform/tts_file_io.cpp
    ${TTS_ROOT}/src/platform/tts_logger.cpp
    ${TTS_ROOT}/src/utils/utils.cpp
    ${TTS_ROOT}/src/modules/iStft.cpp
    ${TTS_ROOT}/src/modules/hann.cpp
    ${TTS_ROOT}/src/modules/attention_encoder.cpp
    ${TTS_ROOT}/src/modules/multi_head_attention.cpp
    ${TTS_ROOT}/src/modules/ffn.cpp
    ${TTS_ROOT}/src/modules/ConvFlow.cpp
    ${TTS_ROOT}/src/modules/DDSConv.cpp
    ${TTS_ROOT}/src/modules/ElementwiseAffine.cpp
    ${TTS_ROOT}/src/modules/random_gen.cpp
    ${TTS_ROOT}/src/modules/ResidualCouplingLayer.cpp
    ${TTS_ROOT}/src/modules/ResBlock1.cpp
    ${TTS_ROOT}/src/modules/WN.cpp
    ${TTS_ROOT}/src/modules/pqmf.cpp
    ${TTS_ROOT}/src/models/TextEncoder.cpp
    ${TTS_ROOT}/src/models/StochasticDurationPredictor.cpp
    ${TTS_ROOT}/src/models/FixDurationPredictor.cpp
    ${TTS_ROOT}/src/models/DurationPredictor_base.cpp
    ${TTS_ROOT}/src/models/ResidualCouplingBlock.cpp
    ${TTS_ROOT}/src/models/Generator_base.cpp
    ${TTS_ROOT}/src/models/Generator_hifigan.cpp
    ${TTS_ROOT}/src/models/Generator_MS.cpp
    ${TTS_ROOT}/src/models/Generator_Istft.cpp
    ${TTS_ROOT}/src/models/Generator_MBB.cpp
    ${TTS_ROOT}/src/models/SynthesizerTrn.cpp
)

add_executable(tts_node tts_node.cpp ${ENGINE_SOURCES})

target_include_directories(tts_node PUBLIC
    ${TTS_ROOT}/eigen-3.4.0
    ${TTS_ROOT}/src/tn/header
    ${TTS_ROOT}/include
    ${TTS_ROOT}/src/header
    ${VOX_COMMON_DIR}
)

target_link_libraries(tts_node PRIVATE pthread zmq)
