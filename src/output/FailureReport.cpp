#include "output/FailureReport.hpp"

#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>

#include "core/Version.hpp"
#include "host/Codec.hpp"

namespace mimicry::output {

namespace {

// FNV-1a, 64 bit.
struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void add(const void* data, std::size_t n) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    void add(std::string_view s) {
        add(s.data(), s.size());
        add("\0", 1);
    }
};

}  // namespace

void FailureReport::set_params(const std::vector<host::ParamDesc>& table) {
    params.clear();
    for (std::size_t i = 0; i < table.size(); ++i)
        params.push_back({table[i].id, table[i].name, i < job.coords.size() ? job.coords[i] : table[i].default_coord, {}});
}

std::string FailureReport::hash() const {
    Fnv f;
    f.add(host::to_string(info.format));
    f.add(info.uid);
    f.add(info.version_hash);
    f.add(to_string(outcome));
    if (!minimal.empty()) {
        for (const auto& m : minimal) {
            f.add(&m.id, sizeof m.id);
            f.add(m.name);
        }
    } else {
        for (const auto& p : params) {
            f.add(&p.id, sizeof p.id);
            f.add(&p.value, sizeof p.value);
        }
    }
    return std::format("{:016x}", f.h);
}

std::string FailureReport::file_name() const {
    std::string name = (info.name.empty() ? std::string("plugin") : info.name) + "-" + hash();
    for (auto& c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '.' || c == '_')) c = '_';
    }
    return name + ".json";
}

json::Value FailureReport::to_json() const {
    json::Value v;
    v.set("mimicry", std::string(mimicry::version));
    v.set("format", std::string(host::to_string(info.format)));
    v.set("uid", info.uid);
    v.set("plugin", info.name);
    v.set("vendor", info.vendor);
    v.set("version_hash", info.version_hash);
    v.set("plugin_path", plugin_path);
    v.set("outcome", std::string(to_string(outcome)));
    v.set("message", message);
    v.set("sample_rate", job.sample_rate);
    v.set("block_size", job.block_size);
    json::Array ps;
    for (const auto& p : params) {
        json::Value e;
        e.set("id", p.id);
        e.set("name", p.name);
        e.set("value", p.value);
        if (!p.text.empty()) e.set("text", p.text);
        ps.push_back(std::move(e));
    }
    v.set("params", json::Value(std::move(ps)));
    json::Array ms;
    for (const auto& m : minimal) {
        json::Value e;
        e.set("id", m.id);
        e.set("name", m.name);
        e.set("threshold", m.threshold);
        ms.push_back(std::move(e));
    }
    v.set("minimal", json::Value(std::move(ms)));
    v.set("job", host::to_json(job));  // notes, settle, tail, limits; coords in the order of `params`
    return v;
}

expected<FailureReport, std::string> FailureReport::from_json(const json::Value& v) {
    if (!v.is_object()) return unexpected(std::string("failure report: not a JSON object"));
    FailureReport r;
    const auto& fmt = v["format"].as_string();
    if (fmt == "VST3" || fmt == "vst3") r.info.format = host::PluginFormat::Vst3;
    else if (fmt == "CLAP" || fmt == "clap") r.info.format = host::PluginFormat::Clap;
    else return unexpected("failure report: unknown format '" + fmt + "'");
    r.info.uid = v["uid"].as_string();
    r.info.name = v["plugin"].as_string();
    r.info.vendor = v["vendor"].as_string();
    r.info.version_hash = v["version_hash"].as_string();
    r.plugin_path = v["plugin_path"].as_string();
    auto o = host::outcome_from_string(v["outcome"].as_string());
    if (!o) return unexpected("failure report: " + o.error());
    r.outcome = *o;
    r.message = v["message"].as_string();
    auto job = host::job_from_json(v["job"]);
    if (!job) return unexpected("failure report: " + job.error());
    r.job = std::move(*job);
    for (const auto& e : v["params"].as_array())
        r.params.push_back({static_cast<std::uint32_t>(e["id"].as_number()), e["name"].as_string(), e["value"].as_number(),
                            e["text"].as_string()});
    for (const auto& e : v["minimal"].as_array())
        r.minimal.push_back({static_cast<std::uint32_t>(e["id"].as_number()), e["name"].as_string(), e["threshold"].as_number()});
    return r;
}

expected<std::filesystem::path, std::string> FailureReport::write(const std::filesystem::path& dir) const {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / file_name();
    std::ofstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot write " + path.string());
    f << to_json().dump(2) << '\n';
    if (!f) return unexpected("cannot write " + path.string());
    return path;
}

expected<FailureReport, std::string> FailureReport::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot read " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::parse(ss.str());
    if (!v) return unexpected(path.string() + ": " + v.error());
    return from_json(*v);
}

std::vector<double> FailureReport::coords_for(const std::vector<host::ParamDesc>& table, std::vector<std::string>* missing) const {
    std::vector<double> coords;
    for (const auto& p : table) coords.push_back(p.default_coord);
    for (const auto& e : params) {
        bool found = false;
        for (std::size_t i = 0; i < table.size() && !found; ++i)
            if (table[i].id == e.id && table[i].name == e.name) {
                coords[i] = e.value;
                found = true;
            }
        if (!found && missing) missing->push_back(std::format("{} (id {})", e.name, e.id));
    }
    return coords;
}

}  // namespace mimicry::output
