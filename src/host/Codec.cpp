#include "host/Codec.hpp"

#include <format>

namespace mimicry::host {

json::Value to_json(const Job& job) {
    json::Array coords, notes;
    for (const double c : job.coords) coords.emplace_back(c);
    for (const Note& n : job.notes) {
        json::Value v;
        v.set("key", n.key).set("cents", n.cents).set("velocity", static_cast<double>(n.velocity))
            .set("on_ms", n.on_ms).set("off_ms", n.off_ms);
        notes.push_back(std::move(v));
    }
    json::Value v;
    v.set("coords", std::move(coords)).set("notes", std::move(notes)).set("sample_rate", job.sample_rate)
        .set("block_size", job.block_size).set("settle_ms", job.settle_ms).set("tail_max_ms", job.tail_max_ms)
        .set("peak_limit", job.peak_limit).set("allow_silent", job.allow_silent);
    return v;
}

expected<Job, std::string> job_from_json(const json::Value& v) {
    if (!v.is_object()) return unexpected(std::string("job: not an object"));
    Job job;
    for (const auto& c : v["coords"].as_array()) job.coords.push_back(c.as_number());
    for (const auto& n : v["notes"].as_array()) {
        Note note;
        note.key = static_cast<int>(n["key"].as_number(60));
        note.cents = n["cents"].as_number();
        note.velocity = static_cast<float>(n["velocity"].as_number(0.8));
        note.on_ms = n["on_ms"].as_number();
        note.off_ms = n["off_ms"].as_number();
        job.notes.push_back(note);
    }
    job.sample_rate = static_cast<int>(v["sample_rate"].as_number(48000));
    job.block_size = static_cast<int>(v["block_size"].as_number(512));
    job.settle_ms = v["settle_ms"].as_number(250);
    job.tail_max_ms = v["tail_max_ms"].as_number(8000);
    job.peak_limit = v["peak_limit"].as_number(50);
    job.allow_silent = v["allow_silent"].as_bool();
    if (job.sample_rate < 8000 || job.sample_rate > 384000) return unexpected(std::string("job: bad sample rate"));
    if (job.block_size < 1 || job.block_size > 65536) return unexpected(std::string("job: bad block size"));
    return job;
}

json::Value to_json(const PluginInfo& info) {
    json::Value v;
    v.set("format", to_string(info.format)).set("uid", info.uid).set("name", info.name).set("vendor", info.vendor)
        .set("version_hash", info.version_hash)
        .set("kind", info.kind == PluginKind::Instrument ? "instrument" : info.kind == PluginKind::Effect ? "effect" : "unknown");
    return v;
}

PluginInfo info_from_json(const json::Value& v) {
    PluginInfo info;
    info.format = v["format"].as_string() == "VST3" ? PluginFormat::Vst3 : PluginFormat::Clap;
    info.uid = v["uid"].as_string();
    info.name = v["name"].as_string();
    info.vendor = v["vendor"].as_string();
    info.version_hash = v["version_hash"].as_string();
    const auto& k = v["kind"].as_string();
    info.kind = k == "instrument" ? PluginKind::Instrument : k == "effect" ? PluginKind::Effect : PluginKind::Unknown;
    return info;
}

json::Value to_json(const ParamDesc& p) {
    json::Array texts;
    for (const auto& t : p.texts) texts.emplace_back(t);
    json::Value flags;
    flags.set("hidden", p.flags.hidden).set("readonly", p.flags.readonly).set("bypass", p.flags.bypass)
        .set("program_change", p.flags.program_change).set("automatable", p.flags.automatable);
    json::Value v;
    v.set("id", p.id).set("name", p.name).set("module", p.module).set("units", p.units).set("steps", p.steps)
        .set("min", p.min).set("max", p.max).set("default_plain", p.default_plain).set("log_scale", p.log_scale)
        .set("default_coord", p.default_coord).set("texts", std::move(texts)).set("flags", std::move(flags));
    return v;
}

ParamDesc param_from_json(const json::Value& v) {
    ParamDesc p;
    p.id = static_cast<std::uint32_t>(v["id"].as_number());
    p.name = v["name"].as_string();
    p.module = v["module"].as_string();
    p.units = v["units"].as_string();
    p.steps = static_cast<int>(v["steps"].as_number());
    p.min = v["min"].as_number();
    p.max = v["max"].as_number(1);
    p.default_plain = v["default_plain"].as_number();
    p.log_scale = v["log_scale"].as_bool();
    p.default_coord = v["default_coord"].as_number();
    const auto& texts = v["texts"].as_array();
    for (std::size_t i = 0; i < p.texts.size() && i < texts.size(); ++i) p.texts[i] = texts[i].as_string();
    const auto& f = v["flags"];
    p.flags.hidden = f["hidden"].as_bool();
    p.flags.readonly = f["readonly"].as_bool();
    p.flags.bypass = f["bypass"].as_bool();
    p.flags.program_change = f["program_change"].as_bool();
    p.flags.automatable = f["automatable"].as_bool(true);
    return p;
}

expected<Outcome, std::string> outcome_from_string(std::string_view name) {
    for (const Outcome o : {Outcome::Ok, Outcome::Silent, Outcome::NonFinite, Outcome::Overload, Outcome::DcRunaway,
                            Outcome::HostError, Outcome::Timeout, Outcome::Crash})
        if (to_string(o) == name) return o;
    return unexpected(std::format("unknown outcome '{}'", name));
}

}  // namespace mimicry::host
