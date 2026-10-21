#include "output/PresetExport.hpp"

#include <fstream>

namespace mimicry::output {

json::Value params_json(const host::PluginInfo& info, const std::vector<host::ParamDesc>& params,
                        const std::vector<double>& coords, const ValueText& text) {
    json::Value v;
    v.set("format", std::string(host::to_string(info.format)));
    v.set("plugin", info.name);
    v.set("uid", info.uid);
    v.set("version_hash", info.version_hash);
    json::Array ps;
    for (std::size_t i = 0; i < params.size() && i < coords.size(); ++i) {
        json::Value e;
        e.set("id", params[i].id);
        e.set("name", params[i].name);
        e.set("module", params[i].module);
        e.set("normalised", coords[i]);
        e.set("display", text ? text(params[i].id, coords[i]) : std::string());
        ps.push_back(std::move(e));
    }
    v.set("params", json::Value(std::move(ps)));
    return v;
}

expected<void, std::string> write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (f) f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return unexpected("cannot write " + path.string());
    return {};
}

expected<void, std::string> write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
    if (!f) return unexpected("cannot write " + path.string());
    return {};
}

}  // namespace mimicry::output
