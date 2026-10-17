#include "host/FileHash.hpp"

#include <cstdint>
#include <format>
#include <fstream>
#include <vector>

namespace mimicry::host {

std::string plugin_version_hash(const std::filesystem::path& plugin) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path file = plugin;
    if (fs::is_directory(plugin, ec)) {
        std::uintmax_t best = 0;
        for (fs::recursive_directory_iterator it(plugin, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            const auto size = it->file_size(e2);
            if (!e2 && size >= best) {
                best = size;
                file = it->path();
            }
        }
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) return "unknown";
    std::uint64_t h = 0xcbf29ce484222325ull;
    std::uint64_t size = 0;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        for (std::size_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 0x100000001b3ull;
        }
        size += n;
    }
    return std::format("{:x}-{:016x}", size, h);
}

}  // namespace mimicry::host
