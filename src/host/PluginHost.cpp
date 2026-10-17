#include "host/PluginHost.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <thread>

#include "host/Engine.hpp"
#include "host/Renderer.hpp"

namespace mimicry::host {

std::string_view to_string(PluginFormat format) noexcept {
    switch (format) {
        case PluginFormat::Clap: return "CLAP";
        case PluginFormat::Vst3: return "VST3";
    }
    return "unknown";
}

expected<PluginFormat, std::string> format_from_path(const std::filesystem::path& path) {
    auto name = path.filename();
    if (name.empty()) name = path.parent_path().filename();  // trailing separator on a bundle directory
    std::string ext = name.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".clap") return PluginFormat::Clap;
    if (ext == ".vst3") return PluginFormat::Vst3;
    return unexpected(std::format("'{}': not a .clap or .vst3 plugin", path.string()));
}

expected<std::unique_ptr<Instance>, HostError> open(const std::filesystem::path& path, std::string_view uid) {
    const auto format = format_from_path(path);
    if (!format) return unexpected(HostError{format.error()});
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return unexpected(HostError{std::format("'{}' does not exist", path.string())});

    auto roles = std::make_shared<ThreadRoles>();
    roles->main = std::this_thread::get_id();
    auto engine = *format == PluginFormat::Clap ? open_clap_engine(path, uid, roles) : open_vst3_engine(path, uid, roles);
    if (!engine) return unexpected(engine.error());

    auto renderer = std::make_unique<Renderer>(std::move(*engine), roles);
    if (auto r = renderer->initialise(); !r) return unexpected(r.error());
    return std::unique_ptr<Instance>(std::move(renderer));
}

}  // namespace mimicry::host
