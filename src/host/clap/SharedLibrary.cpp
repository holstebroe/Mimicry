#include "host/clap/SharedLibrary.hpp"

#include <format>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mimicry::host::clap {

SharedLibrary::~SharedLibrary() {
    if (!handle_) return;
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
}

SharedLibrary& SharedLibrary::operator=(SharedLibrary&& o) noexcept {
    if (this != &o) {
        this->~SharedLibrary();
        handle_ = o.handle_;
        o.handle_ = nullptr;
    }
    return *this;
}

expected<SharedLibrary, std::string> SharedLibrary::open(const std::filesystem::path& path) {
    std::filesystem::path binary = path;
#if defined(__APPLE__)
    // A .clap on macOS is a bundle: Contents/MacOS/<executable>.
    if (std::filesystem::is_directory(path)) {
        const auto macos = path / "Contents" / "MacOS";
        std::error_code ec;
        binary.clear();
        for (const auto& e : std::filesystem::directory_iterator(macos, ec))
            if (e.is_regular_file()) {
                binary = e.path();
                break;
            }
        if (binary.empty()) return unexpected(std::format("'{}': no executable in the bundle", path.string()));
    }
#endif
    SharedLibrary lib;
#if defined(_WIN32)
    lib.handle_ = LoadLibraryW(binary.wstring().c_str());
    if (!lib.handle_) return unexpected(std::format("LoadLibrary('{}') failed ({})", path.string(), GetLastError()));
#else
    lib.handle_ = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib.handle_) {
        const char* e = dlerror();
        return unexpected(std::format("dlopen('{}'): {}", path.string(), e ? e : "failed"));
    }
#endif
    return lib;
}

void* SharedLibrary::symbol(const char* name) const {
    if (!handle_) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

}  // namespace mimicry::host::clap
