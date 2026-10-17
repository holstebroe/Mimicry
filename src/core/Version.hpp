#pragma once

#include <string_view>

// MIMICRY_VERSION is defined by CMake (project version) for everything that links mimicry_core.
#ifndef MIMICRY_VERSION
#define MIMICRY_VERSION "0.0.0"
#endif

namespace mimicry {

inline constexpr std::string_view version = MIMICRY_VERSION;

}  // namespace mimicry
