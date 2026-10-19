#include "opt/Profile.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace mimicry::opt {

std::string_view to_string(ProbeClass c) noexcept {
    switch (c) {
        case ProbeClass::Dead: return "dead";
        case ProbeClass::Pitch: return "pitch";
        case ProbeClass::Brightness: return "brightness";
        case ProbeClass::Time: return "time";
        case ProbeClass::Level: return "level";
        case ProbeClass::Spectrum: return "spectrum";
        case ProbeClass::Fx: return "fx";
    }
    return "spectrum";
}

std::optional<ProbeClass> parse_probe_class(std::string_view text) noexcept {
    for (int c = 0; c <= int(ProbeClass::Fx); ++c)
        if (to_string(ProbeClass(c)) == text) return ProbeClass(c);
    return std::nullopt;
}

ParamGroup group_of(ProbeClass c) noexcept {
    switch (c) {
        case ProbeClass::Pitch: return ParamGroup::Osc;
        case ProbeClass::Brightness: return ParamGroup::Filter;
        case ProbeClass::Time: return ParamGroup::Env;
        case ProbeClass::Level: return ParamGroup::Amp;
        case ProbeClass::Fx: return ParamGroup::Fx;
        case ProbeClass::Dead:
        case ProbeClass::Spectrum: break;
    }
    return ParamGroup::Unknown;
}

namespace {

std::string_view response_name(Response r) {
    return r == Response::Log ? "log" : r == Response::Exp ? "exp" : "linear";
}

Response parse_response(std::string_view s) {
    return s == "log" ? Response::Log : s == "exp" ? Response::Exp : Response::Linear;
}

}  // namespace

const ParamProfile* Profile::find(const host::ParamDesc& p) const {
    for (const auto& e : params)
        if (e.id == p.id && e.name == p.name) return &e;
    return nullptr;
}

bool Profile::matches(const host::PluginInfo& info) const {
    return info.format == format && info.uid == uid && info.version_hash == version_hash;
}

bool Profile::separable(std::uint32_t a, std::uint32_t b) const {
    for (const auto& p : pairs)
        if ((p.a == a && p.b == b) || (p.a == b && p.b == a)) return p.separable;
    return true;
}

json::Value Profile::to_json() const {
    json::Value v;
    v.set("format", std::string(host::to_string(format)));
    v.set("uid", uid);
    v.set("name", name);
    v.set("vendor", vendor);
    v.set("version_hash", version_hash);
    v.set("floor_db", floor_db);
    v.set("embedding_floor", embedding_floor);
    v.set("render_ms", render_ms);
    json::Array ps;
    for (const auto& p : params) {
        json::Value e;
        e.set("id", p.id);
        e.set("name", p.name);
        e.set("group", std::string(to_string(p.names.group)));
        e.set("confidence", p.names.confidence);
        e.set("class", std::string(to_string(p.measured)));
        e.set("effect", p.effect);
        e.set("response", std::string(response_name(p.response)));
        json::Array safe{json::Value(p.safe_lo), json::Value(p.safe_hi)};
        e.set("safe", json::Value(std::move(safe)));
        json::Array oc;
        for (auto o : p.outcomes) oc.push_back(std::string(to_string(o)));
        e.set("outcomes", json::Value(std::move(oc)));
        ps.push_back(std::move(e));
    }
    v.set("params", json::Value(std::move(ps)));
    json::Array pr;
    for (const auto& p : pairs) {
        json::Value e;
        e.set("a", p.a);
        e.set("b", p.b);
        e.set("interaction", p.interaction);
        e.set("separable", p.separable);
        pr.push_back(std::move(e));
    }
    v.set("pairs", json::Value(std::move(pr)));
    v.set("constraints", ConstraintSet::to_json(constraints));
    json::Value pc;
    pc.set("method", std::string(to_string(pitch.method)));
    pc.set("bend_range", pitch.bend_range);
    pc.set("tuning_cents", pitch.tuning_cents);
    pc.set("bend_cents", pitch.bend_cents);
    if (!pitch.note.empty()) pc.set("note", pitch.note);
    v.set("pitch", std::move(pc));
    return v;
}

expected<Profile, std::string> Profile::from_json(const json::Value& v) {
    if (!v.is_object()) return unexpected(std::string("profile: not a JSON object"));
    Profile p;
    const auto& fmt = v["format"].as_string();
    if (fmt == "VST3" || fmt == "vst3") p.format = host::PluginFormat::Vst3;
    else if (fmt == "CLAP" || fmt == "clap") p.format = host::PluginFormat::Clap;
    else return unexpected("profile: unknown format '" + fmt + "'");
    p.uid = v["uid"].as_string();
    p.name = v["name"].as_string();
    p.vendor = v["vendor"].as_string();
    p.version_hash = v["version_hash"].as_string();
    p.floor_db = v["floor_db"].as_number();
    p.embedding_floor = v["embedding_floor"].as_number();
    p.render_ms = v["render_ms"].as_number();
    for (const auto& e : v["params"].as_array()) {
        ParamProfile q;
        q.id = static_cast<std::uint32_t>(e["id"].as_number());
        q.name = e["name"].as_string();
        q.names.group = parse_group(e["group"].as_string());
        q.names.confidence = e["confidence"].as_number();
        q.measured = parse_probe_class(e["class"].as_string()).value_or(ProbeClass::Spectrum);
        q.effect = e["effect"].as_number();
        q.response = parse_response(e["response"].as_string());
        const auto& safe = e["safe"].as_array();
        if (safe.size() == 2) {
            q.safe_lo = safe[0].as_number(0);
            q.safe_hi = safe[1].as_number(1);
        }
        for (const auto& o : e["outcomes"].as_array()) q.outcomes.push_back(parse_outcome(o.as_string()));
        p.params.push_back(std::move(q));
    }
    for (const auto& e : v["pairs"].as_array()) {
        PairProfile q;
        q.a = static_cast<std::uint32_t>(e["a"].as_number());
        q.b = static_cast<std::uint32_t>(e["b"].as_number());
        q.interaction = e["interaction"].as_number();
        q.separable = e["separable"].as_bool(true);
        p.pairs.push_back(q);
    }
    auto cs = ConstraintSet::from_json(v["constraints"]);
    if (!cs) return unexpected("profile: " + cs.error());
    p.constraints = std::move(*cs);
    const auto& pc = v["pitch"];
    p.pitch.method = parse_pitch_method(pc["method"].as_string());
    p.pitch.bend_range = pc["bend_range"].as_number();
    p.pitch.tuning_cents = pc["tuning_cents"].as_number();
    p.pitch.bend_cents = pc["bend_cents"].as_number();
    p.pitch.note = pc["note"].as_string();
    return p;
}

expected<void, std::string> Profile::save(const std::filesystem::path& path) const {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot write " + path.string());
    f << to_json().dump(2) << '\n';
    if (!f) return unexpected("cannot write " + path.string());
    return {};
}

expected<Profile, std::string> Profile::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot read " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::parse(ss.str());
    if (!v) return unexpected(path.string() + ": " + v.error());
    return from_json(*v);
}

std::filesystem::path plugin_file_path(const std::filesystem::path& dir, const host::PluginInfo& info,
                                       std::string_view extension) {
    std::string name = std::string(host::to_string(info.format)) + "-" + info.uid + "-" + info.version_hash;
    for (auto& c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '.' || c == '_')) c = '_';
    }
    return dir / (name + std::string(extension));
}

std::filesystem::path Profile::default_path(const std::filesystem::path& dir, const host::PluginInfo& info) {
    return plugin_file_path(dir, info, ".json");
}

}  // namespace mimicry::opt
