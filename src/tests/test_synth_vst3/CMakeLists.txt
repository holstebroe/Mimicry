# mimicry_test_synth as a VST3 plugin through clap-wrapper (docs/spec/07_OUTPUT_VALIDATION.md section 5).
# A separate CMake project, built from the main one with ExternalProject: clap-wrapper compiles its own
# copy of the VST3 SDK sources, which must not collide with the SDK targets of the host build.
cmake_minimum_required(VERSION 3.21)
project(mimicry_test_synth_vst3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CLAP_SDK_ROOT ${CLAP_ROOT} CACHE PATH "")
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "")
set(CLAP_WRAPPER_BUILD_AUV2 OFF CACHE BOOL "")
set(CLAP_WRAPPER_DONT_ADD_TARGETS ON CACHE BOOL "")

include(FetchContent)
FetchContent_Declare(clap_wrapper
    GIT_REPOSITORY https://github.com/holstebroe/clap-wrapper.git
    GIT_TAG 98bed603c0dece1922c7be6c9c1e9d5935e9c898  # vst3-midi-out (plan WP1.6)
    GIT_SHALLOW OFF)
FetchContent_MakeAvailable(clap_wrapper)

add_library(test_synth_impl INTERFACE)
target_include_directories(test_synth_impl INTERFACE ${CLAP_ROOT}/include ${SRC_ROOT}/tests/test_synth)

make_clapfirst_plugins(
    TARGET_NAME mimicry_test_synth
    IMPL_TARGET test_synth_impl
    OUTPUT_NAME "mimicry_test_synth"
    ENTRY_SOURCE ${SRC_ROOT}/tests/test_synth/TestSynthClap.cpp
    BUNDLE_IDENTIFIER org.mimicry.test-synth
    BUNDLE_VERSION 0.1.0
    PLUGIN_FORMATS CLAP VST3
    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/assets)
