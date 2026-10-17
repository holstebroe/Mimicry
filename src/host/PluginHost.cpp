#include "host/PluginHost.hpp"

#include <algorithm>
#include <cctype>
#include <format>

namespace mimicry::host {

std::string_view to_string(PluginFormat format) noexcept {
    switch (format) {
        case PluginFormat::Clap: return "CLAP";
        case PluginFormat::Vst3: return "VST3";
    }
    return "unknown";
}

std::expected<PluginFormat, std::string> format_from_path(const std::filesystem::path& path) {
    auto name = path.filename();
    if (name.empty()) name = path.parent_path().filename();  // trailing separator on a bundle directory
    std::string ext = name.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".clap") return PluginFormat::Clap;
    if (ext == ".vst3") return PluginFormat::Vst3;
    return std::unexpected(std::format("'{}': not a .clap or .vst3 plugin", path.string()));
}

std::expected<std::unique_ptr<Instance>, HostError> open(const std::filesystem::path& path,
                                                         std::string_view /*uid*/) {
    const auto format = format_from_path(path);
    if (!format) return std::unexpected(HostError{format.error()});
    return std::unexpected(HostError{std::format("{} hosting is not implemented yet (see docs/PROJECT_PLAN.md)",
                                                 to_string(*format))});
}

}  // namespace mimicry::host
