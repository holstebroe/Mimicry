#pragma once
// Plugin version hash: file size plus FNV-1a over the content (docs/spec/02_HOSTING.md section 4). For a
// bundle directory the largest regular file inside it is hashed. Neither format guarantees a
// trustworthy version string, so profiles key on this.

#include <filesystem>
#include <string>

namespace mimicry::host {

[[nodiscard]] std::string plugin_version_hash(const std::filesystem::path& plugin);

}  // namespace mimicry::host
