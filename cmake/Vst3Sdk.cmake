# VST3 SDK for hosting (docs/spec/02_HOSTING.md section 8). MIT licensed since 3.8 (plan OQ7).
# Taken from VST3_SDK_ROOT if set, else fetched at configure time. Defines MIMICRY_HAVE_VST3 and the
# targets sdk_hosting / sdk / base / pluginterfaces.

set(MIMICRY_HAVE_VST3 OFF)
if(NOT MIMICRY_VST3_HOSTING)
    return()
endif()

set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
set(SMTG_CREATE_PLUGIN_LINK OFF CACHE BOOL "" FORCE)
set(SMTG_RUN_VST_VALIDATOR OFF CACHE BOOL "" FORCE)

if(VST3_SDK_ROOT)
else()
    include(FetchContent)
    FetchContent_Declare(vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG v3.8.0_build_66
        GIT_SHALLOW ON
        GIT_SUBMODULES "base;pluginterfaces;public.sdk;cmake"
        GIT_SUBMODULES_RECURSE OFF)
    FetchContent_GetProperties(vst3sdk)
    if(NOT vst3sdk_POPULATED)
        FetchContent_Populate(vst3sdk)
    endif()
    # EXCLUDE_FROM_ALL: only the libraries we link are built, not the SDK's validator and samples.
    add_subdirectory(${vst3sdk_SOURCE_DIR} ${vst3sdk_BINARY_DIR} EXCLUDE_FROM_ALL)
    set(MIMICRY_VST3_SDK_DIR ${vst3sdk_SOURCE_DIR})
endif()
if(VST3_SDK_ROOT)
    add_subdirectory(${VST3_SDK_ROOT} ${CMAKE_BINARY_DIR}/vst3sdk EXCLUDE_FROM_ALL)
    set(MIMICRY_VST3_SDK_DIR ${VST3_SDK_ROOT})
endif()
set(MIMICRY_HAVE_VST3 ON)
