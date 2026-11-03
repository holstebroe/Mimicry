#pragma once
// Environment variable access without the MSVC getenv deprecation warning (C4996).

#include <cstdlib>
#include <string>

namespace mimicry {

// Reads an environment variable; empty when unset.
inline std::string env_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

}  // namespace mimicry
