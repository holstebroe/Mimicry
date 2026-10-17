#pragma once
// Loads a plugin binary (CLAP: a shared library, or a macOS bundle). Thin wrapper over
// dlopen / LoadLibrary. Format-specific, so it lives under host/clap.

#include <filesystem>
#include <string>

#include "core/Expected.hpp"

namespace mimicry::host::clap {

class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary();
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;
    SharedLibrary(SharedLibrary&& o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    SharedLibrary& operator=(SharedLibrary&& o) noexcept;

    [[nodiscard]] static expected<SharedLibrary, std::string> open(const std::filesystem::path& path);
    [[nodiscard]] void* symbol(const char* name) const;

private:
    void* handle_ = nullptr;
};

}  // namespace mimicry::host::clap
