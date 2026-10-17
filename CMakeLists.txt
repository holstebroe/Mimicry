cmake_minimum_required(VERSION 3.21)
project(Mimicry VERSION 0.1.0 LANGUAGES C CXX)

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
    src/core/Outcome.cpp
)
target_include_directories(mimicry_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(mimicry_core PUBLIC MIMICRY_VERSION=\"${PROJECT_VERSION}\")
mimicry_warnings(mimicry_core)

add_library(mimicry_host STATIC
    src/host/PluginHost.cpp
)
target_link_libraries(mimicry_host PUBLIC mimicry_core)
mimicry_warnings(mimicry_host)

# --- Executables -------------------------------------------------------------------------------

# Supervisor / command line.
add_executable(mimicry src/app/main.cpp)
target_link_libraries(mimicry PRIVATE mimicry_host mimicry_core)
mimicry_warnings(mimicry)

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

# --- Tests -------------------------------------------------------------------------------------

if(MIMICRY_BUILD_TESTS)
    enable_testing()
    add_executable(mimicry_smoke_test src/tests/smoke_test_main.cpp)
    target_link_libraries(mimicry_smoke_test PRIVATE mimicry_host mimicry_core)
    mimicry_warnings(mimicry_smoke_test)
    add_test(NAME smoke COMMAND mimicry_smoke_test)
endif()
