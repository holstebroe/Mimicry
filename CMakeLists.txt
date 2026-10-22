cmake_minimum_required(VERSION 3.21)
project(Mimicry VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

# C++20 (docs/spec/01_ARCHITECTURE.md section 3). std::expected is shimmed in src/core/Expected.hpp.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# VST3 hosting (docs/spec/02_HOSTING.md section 8). The backend is not written yet (WP1.7), so for now the
# option only records the intent; the VST3 SDK is fetched, or taken from VST3_SDK_ROOT, once it is used.
option(MIMICRY_VST3_HOSTING "Host VST3 plugins (needs the VST3 SDK)" ON)
option(MIMICRY_BUILD_TESTS "Build the test executables" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Vst3Sdk)

# Link-time optimisation in Release builds (as in Acidus and Tannhauser).
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported AND CMAKE_BUILD_TYPE MATCHES "Release")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
if(MSVC)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()

function(mimicry_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# --- Libraries ---------------------------------------------------------------------------------

add_library(mimicry_core STATIC
    src/core/Cma.cpp
    src/core/Fft.cpp
    src/core/Ipc.cpp
    src/core/Json.cpp
    src/core/Outcome.cpp
    src/core/ParamSemantics.cpp
    src/core/Sampling.cpp
    src/core/Wav.cpp
)
target_include_directories(mimicry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(mimicry_core PUBLIC rt)  # shm_open on glibc < 2.34
endif()
target_compile_definitions(mimicry_core PUBLIC MIMICRY_VERSION=\"${PROJECT_VERSION}\")
mimicry_warnings(mimicry_core)

add_library(mimicry_features STATIC
    src/features/Features.cpp
    src/features/Loss.cpp
    src/features/Pitch.cpp
    src/features/Preprocess.cpp
    src/features/Spectral.cpp
)
target_link_libraries(mimicry_features PUBLIC mimicry_core)
mimicry_warnings(mimicry_features)

add_library(mimicry_host STATIC
    src/host/Codec.cpp
    src/host/FileHash.cpp
    src/host/MessagePump.cpp
    src/host/ParamUtil.cpp
    src/host/PluginHost.cpp
    src/host/Renderer.cpp
)
find_package(Threads REQUIRED)
target_link_libraries(mimicry_host PUBLIC mimicry_core Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
    target_link_libraries(mimicry_host PUBLIC "-framework CoreFoundation")
endif()
mimicry_warnings(mimicry_host)

# VST3 backend: SDK hosting classes (cmake/Vst3Sdk.cmake).
if(MIMICRY_HAVE_VST3)
    target_sources(mimicry_host PRIVATE src/host/vst3/Vst3Engine.cpp)
    target_include_directories(mimicry_host SYSTEM PRIVATE ${MIMICRY_VST3_SDK_DIR})
    target_link_libraries(mimicry_host PRIVATE sdk_hosting sdk_common sdk base pluginterfaces)
    # Hosting sources the SDK's own libraries leave out: the per-platform module loader, the plug provider
    # and the memory stream.
    set(_h ${MIMICRY_VST3_SDK_DIR}/public.sdk/source)
    target_sources(mimicry_host PRIVATE ${_h}/vst/hosting/plugprovider.cpp ${_h}/common/memorystream.cpp)
    if(WIN32)
        target_sources(mimicry_host PRIVATE ${_h}/vst/hosting/module_win32.cpp)
        # The SDK assumes u8string() returns std::string; C++20 makes it std::u8string.
        set_source_files_properties(${_h}/vst/hosting/module_win32.cpp PROPERTIES
            COMPILE_OPTIONS "/Zc:char8_t-" COMPILE_DEFINITIONS _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING)
    elseif(APPLE)
        target_sources(mimicry_host PRIVATE ${_h}/vst/hosting/module_mac.mm)
        set_source_files_properties(${_h}/vst/hosting/module_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
        target_link_libraries(mimicry_host PUBLIC "-framework Foundation")
    else()
        target_sources(mimicry_host PRIVATE ${_h}/vst/hosting/module_linux.cpp)
    endif()
else()
    target_sources(mimicry_host PRIVATE src/host/vst3/Vst3EngineStub.cpp)
endif()

# CLAP backend: needs the clap submodule headers (git submodule update --init).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/clap/include/clap/clap.h)
    target_sources(mimicry_host PRIVATE src/host/clap/ClapEngine.cpp src/host/clap/SharedLibrary.cpp)
    target_include_directories(mimicry_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/clap/include)
else()
    message(WARNING "clap submodule not found: CLAP hosting is disabled (git submodule update --init)")
    target_sources(mimicry_host PRIVATE src/host/clap/ClapEngineStub.cpp)
endif()

# Optimizer (docs/spec/04_OPTIMIZER.md): encoding, probe, pipeline, fingerprints, surrogate. Sees only the
# format-agnostic host interface (PluginHost.hpp); renders go through a RenderBatch (a worker pool).
add_library(mimicry_opt STATIC
    src/opt/Checkpoint.cpp
    src/opt/Constraints.cpp
    src/opt/Embedding.cpp
    src/opt/Fingerprint.cpp
    src/opt/Objective.cpp
    src/opt/PitchCalibration.cpp
    src/opt/Pipeline.cpp
    src/opt/Probe.cpp
    src/opt/Profile.cpp
    src/opt/Space.cpp
    src/opt/Surrogate.cpp
)
target_link_libraries(mimicry_opt PUBLIC mimicry_features mimicry_host mimicry_core)
mimicry_warnings(mimicry_opt)

# Supervisor side of the worker protocol. Includes only the Instance interface, never a plugin loader.
add_library(mimicry_supervisor STATIC
    src/supervisor/WorkerClient.cpp
    src/supervisor/WorkerPool.cpp
)
target_link_libraries(mimicry_supervisor PUBLIC mimicry_host mimicry_core)
mimicry_warnings(mimicry_supervisor)

# Run output (docs/spec/07_OUTPUT_VALIDATION.md): failure reports (spec 05 section 7) and preset files.
add_library(mimicry_output STATIC
    src/output/FailureReport.cpp
    src/output/PresetExport.cpp
    src/output/Ppm.cpp
    src/output/VstPreset.cpp
)
target_link_libraries(mimicry_output PUBLIC mimicry_host mimicry_core)
mimicry_warnings(mimicry_output)

# Targets (docs/spec/06_TARGETS.md): the preset harvest so far (section 6).
add_library(mimicry_targets STATIC
    src/targets/Harvest.cpp
)
target_link_libraries(mimicry_targets PUBLIC mimicry_opt mimicry_output mimicry_host mimicry_core)
mimicry_warnings(mimicry_targets)

# Front end logic (docs/spec/09_GUI.md): forms, run model, child process. No windows, so it builds and is
# tested everywhere; the Windows program on top of it is mimicry_gui.
add_library(mimicry_gui_core STATIC
    src/gui/ChildProcess.cpp
    src/gui/Forms.cpp
    src/gui/RunModel.cpp
)
target_link_libraries(mimicry_gui_core PUBLIC mimicry_core Threads::Threads)
mimicry_warnings(mimicry_gui_core)

# --- Executables -------------------------------------------------------------------------------

# Supervisor / command line.
add_executable(mimicry
    src/app/main.cpp
    src/app/Cli.cpp
    src/app/CmdFit.cpp
    src/app/CmdHarvest.cpp
    src/app/CmdTransfer.cpp
    src/app/Console.cpp
    src/app/FitRun.cpp
    src/app/RunDir.cpp)
target_include_directories(mimicry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(mimicry PRIVATE mimicry_targets mimicry_opt mimicry_output mimicry_supervisor mimicry_host mimicry_core)
mimicry_warnings(mimicry)

# Windows front end (docs/spec/09_GUI.md): a Win32 program that runs mimicry.exe --events as a child process. It loads
# no plugin and includes no CLAP or VST3 header (CLAUDE.md rules 3, 4). Other platforms use the command line.
if(WIN32)
    add_executable(mimicry_gui WIN32
        src/gui/win/MainWindow.cpp
        src/gui/win/FormPanel.cpp
        src/gui/win/LossGraph.cpp)
    target_link_libraries(mimicry_gui PRIVATE mimicry_gui_core comctl32 comdlg32 shell32 ole32 user32 gdi32 winmm)
    target_include_directories(mimicry_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    mimicry_warnings(mimicry_gui)
    add_dependencies(mimicry_gui mimicry mimicry_worker)
endif()

# Render worker: the only process that loads plugins (docs/spec/05_RESILIENCE.md section 1).
add_executable(mimicry_worker src/worker/worker_main.cpp)
target_link_libraries(mimicry_worker PRIVATE mimicry_host mimicry_core)
mimicry_warnings(mimicry_worker)

# --- Test plugin (stub) ------------------------------------------------------------------------
# docs/spec/07_OUTPUT_VALIDATION.md section 5. Needs the clap submodule (git submodule update --init).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/clap/include/clap/clap.h)
    add_library(mimicry_test_synth MODULE src/tests/test_synth/TestSynthClap.cpp)
    target_include_directories(mimicry_test_synth PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/clap/include)
    set_target_properties(mimicry_test_synth PROPERTIES PREFIX "" SUFFIX ".clap"
        OUTPUT_NAME "mimicry_test_synth")
    if(UNIX AND NOT APPLE)
        # Keep the plugin's symbols to itself; only clap_entry is exported.
        set_target_properties(mimicry_test_synth PROPERTIES CXX_VISIBILITY_PRESET hidden
            C_VISIBILITY_PRESET hidden)
    endif()
    mimicry_warnings(mimicry_test_synth)
else()
    message(STATUS "clap submodule not found: skipping mimicry_test_synth")
endif()

# --- Test plugin as VST3 (clap-wrapper) ----------------------------------------------------------
# A separate project (src/tests/test_synth_vst3), built from here so both backends test the same code.
set(MIMICRY_TEST_SYNTH_VST3 "")
if(MIMICRY_HAVE_VST3 AND TARGET mimicry_test_synth AND MIMICRY_BUILD_TESTS)
    include(ExternalProject)
    set(_tsv_bin ${CMAKE_BINARY_DIR}/test_synth_vst3)
    ExternalProject_Add(mimicry_test_synth_vst3_build
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/tests/test_synth_vst3
        BINARY_DIR ${_tsv_bin}
        CMAKE_ARGS -DCLAP_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/clap -DSRC_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/src
                   -DVST3_SDK_ROOT=${MIMICRY_VST3_SDK_DIR} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target mimicry_test_synth_vst3 --config ${CMAKE_BUILD_TYPE}
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        USES_TERMINAL_BUILD ON)
    if(WIN32)
        set(MIMICRY_TEST_SYNTH_VST3 ${_tsv_bin}/assets/VST3/mimicry_test_synth.vst3)
    else()
        set(MIMICRY_TEST_SYNTH_VST3 ${_tsv_bin}/assets/mimicry_test_synth.vst3)
    endif()
endif()

# --- Tests -------------------------------------------------------------------------------------

if(MIMICRY_BUILD_TESTS)
    enable_testing()
    add_executable(mimicry_smoke_test src/tests/smoke_test_main.cpp)
    target_link_libraries(mimicry_smoke_test PRIVATE mimicry_host mimicry_core)
    mimicry_warnings(mimicry_smoke_test)
    add_test(NAME smoke COMMAND mimicry_smoke_test)

    add_executable(mimicry_features_test src/tests/features_test_main.cpp)
    target_link_libraries(mimicry_features_test PRIVATE mimicry_features mimicry_host mimicry_core)
    mimicry_warnings(mimicry_features_test)
    add_test(NAME features COMMAND mimicry_features_test)

    add_executable(mimicry_opt_test src/tests/opt_test_main.cpp)
    target_link_libraries(mimicry_opt_test PRIVATE mimicry_opt)
    mimicry_warnings(mimicry_opt_test)
    add_test(NAME opt COMMAND mimicry_opt_test)

    add_executable(mimicry_output_test src/tests/output_test_main.cpp)
    target_link_libraries(mimicry_output_test PRIVATE mimicry_output)
    target_include_directories(mimicry_output_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    mimicry_warnings(mimicry_output_test)
    add_test(NAME output COMMAND mimicry_output_test)

    add_executable(mimicry_gui_test src/tests/gui_test_main.cpp)
    target_link_libraries(mimicry_gui_test PRIVATE mimicry_gui_core)
    target_include_directories(mimicry_gui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    mimicry_warnings(mimicry_gui_test)
    add_test(NAME gui_core COMMAND mimicry_gui_test)

    add_executable(mimicry_core_test src/tests/core_test_main.cpp)
    target_link_libraries(mimicry_core_test PRIVATE mimicry_core)
    mimicry_warnings(mimicry_core_test)
    add_test(NAME core COMMAND mimicry_core_test)

    if(TARGET mimicry_test_synth)
        add_executable(mimicry_host_test src/tests/host_test_main.cpp)
        target_link_libraries(mimicry_host_test PRIVATE mimicry_host mimicry_core)
        target_include_directories(mimicry_host_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        mimicry_warnings(mimicry_host_test)
        add_dependencies(mimicry_host_test mimicry_test_synth)
        add_test(NAME host_clap COMMAND mimicry_host_test $<TARGET_FILE:mimicry_test_synth>)

        add_executable(mimicry_worker_test src/tests/worker_test_main.cpp)
        target_link_libraries(mimicry_worker_test PRIVATE mimicry_supervisor mimicry_host mimicry_core)
        target_include_directories(mimicry_worker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        mimicry_warnings(mimicry_worker_test)
        add_dependencies(mimicry_worker_test mimicry_test_synth mimicry_worker)
        add_executable(mimicry_fit_test src/tests/fit_test_main.cpp)
        target_link_libraries(mimicry_fit_test PRIVATE mimicry_opt mimicry_output mimicry_supervisor)
        target_include_directories(mimicry_fit_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        mimicry_warnings(mimicry_fit_test)
        add_dependencies(mimicry_fit_test mimicry_test_synth mimicry_worker mimicry)
        add_test(NAME fit_clap COMMAND mimicry_fit_test $<TARGET_FILE:mimicry_test_synth> $<TARGET_FILE:mimicry_worker> $<TARGET_FILE:mimicry>)

        # T17: preset harvest (docs/spec/06_TARGETS.md section 6).
        add_executable(mimicry_harvest_test src/tests/harvest_test_main.cpp)
        target_link_libraries(mimicry_harvest_test PRIVATE mimicry_targets mimicry_supervisor)
        target_include_directories(mimicry_harvest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        mimicry_warnings(mimicry_harvest_test)
        add_dependencies(mimicry_harvest_test mimicry_test_synth mimicry_worker mimicry)
        add_test(NAME harvest_clap COMMAND mimicry_harvest_test $<TARGET_FILE:mimicry_test_synth> $<TARGET_FILE:mimicry_worker> $<TARGET_FILE:mimicry>)

        # The command line as a front end drives it: events, cancel, resume, transfer (docs/spec/09_GUI.md).
        add_executable(mimicry_cli_test src/tests/cli_test_main.cpp)
        target_link_libraries(mimicry_cli_test PRIVATE mimicry_core)
        target_include_directories(mimicry_cli_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        mimicry_warnings(mimicry_cli_test)
        add_dependencies(mimicry_cli_test mimicry_test_synth mimicry_worker mimicry)
        add_dependencies(mimicry_gui_test mimicry_test_synth mimicry_worker mimicry)
        add_test(NAME gui_e2e_clap COMMAND mimicry_gui_test $<TARGET_FILE:mimicry_test_synth> $<TARGET_FILE:mimicry>)
        # Cancel and resume through the command line. Not run on macOS: there the `cancel` file of a run directory is
        # never noticed by the running process (plan OQ17); the front end and the CI target are Windows (owner).
        if(NOT APPLE)
            add_test(NAME cli_clap COMMAND mimicry_cli_test $<TARGET_FILE:mimicry_test_synth> $<TARGET_FILE:mimicry>)
        endif()

        if(MIMICRY_TEST_SYNTH_VST3)
            add_dependencies(mimicry_cli_test mimicry_test_synth_vst3_build)
            if(NOT APPLE)
                add_test(NAME cli_vst3 COMMAND mimicry_cli_test ${MIMICRY_TEST_SYNTH_VST3} $<TARGET_FILE:mimicry>)
                add_test(NAME cli_clap_to_vst3 COMMAND mimicry_cli_test $<TARGET_FILE:mimicry_test_synth> $<TARGET_FILE:mimicry> ${MIMICRY_TEST_SYNTH_VST3})
            endif()
            add_dependencies(mimicry_fit_test mimicry_test_synth_vst3_build)
            add_test(NAME fit_vst3 COMMAND mimicry_fit_test ${MIMICRY_TEST_SYNTH_VST3} $<TARGET_FILE:mimicry_worker> $<TARGET_FILE:mimicry>)
            add_dependencies(mimicry_harvest_test mimicry_test_synth_vst3_build)
            add_test(NAME harvest_vst3 COMMAND mimicry_harvest_test ${MIMICRY_TEST_SYNTH_VST3} $<TARGET_FILE:mimicry_worker> $<TARGET_FILE:mimicry>)
            add_dependencies(mimicry_host_test mimicry_test_synth_vst3_build)
            add_dependencies(mimicry_worker_test mimicry_test_synth_vst3_build)
            add_test(NAME host_vst3 COMMAND mimicry_host_test ${MIMICRY_TEST_SYNTH_VST3})
            add_test(NAME equivalence_clap_vst3 COMMAND mimicry_host_test $<TARGET_FILE:mimicry_test_synth> ${MIMICRY_TEST_SYNTH_VST3})
            add_test(NAME worker_vst3 COMMAND mimicry_worker_test ${MIMICRY_TEST_SYNTH_VST3} $<TARGET_FILE:mimicry_worker>)
        endif()
        add_test(NAME worker_clap COMMAND mimicry_worker_test $<TARGET_FILE:mimicry_test_synth> $<TARGET_FILE:mimicry_worker>)
    endif()
endif()
