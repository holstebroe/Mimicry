#include "opt/Checkpoint.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace mimicry::opt {

namespace {

json::Value vec_json(const std::vector<double>& v) {
    json::Array a;
    for (double d : v) a.push_back(std::isfinite(d) ? json::Value(d) : json::Value(nullptr));
    return json::Value(std::move(a));
}

std::vector<double> vec_from(const json::Value& v) {
    std::vector<double> out;
    for (const auto& e : v.as_array()) out.push_back(e.as_number());
    return out;
}

}  // namespace

bool PipelineState::is_done(int stage) const { return std::find(done.begin(), done.end(), stage) != done.end(); }

json::Value PipelineState::to_json() const {
    json::Value v;
    v.set("version", version).set("dimension", dimension).set("elapsed_s", elapsed_s).set("evals", static_cast<std::int64_t>(evals))
        .set("sessions", sessions).set("best", vec_json(best)).set("best_loss", std::isfinite(best_loss) ? json::Value(best_loss) : json::Value(nullptr));
    json::Array d;
    for (int s : done) d.push_back(s);
    v.set("done", json::Value(std::move(d)));
    json::Array k, f;
    for (const auto& c : kept) k.push_back(vec_json(c));
    for (const auto& c : finalists) f.push_back(vec_json(c));
    v.set("kept", json::Value(std::move(k))).set("finalists", json::Value(std::move(f)));
    json::Array st;
    for (const auto& s : stages) {
        json::Value e;
        e.set("name", s.name).set("free", s.free).set("before", s.before).set("after", s.after)
            .set("evals", static_cast<std::int64_t>(s.evals)).set("seconds", s.seconds);
        st.push_back(std::move(e));
    }
    v.set("stages", json::Value(std::move(st)));
    json::Object oc;
    for (const auto& [o, n] : outcomes) oc[std::string(to_string(o))] = static_cast<std::int64_t>(n);
    v.set("outcomes", json::Value(std::move(oc)));
    v.set("resampled", static_cast<std::int64_t>(resampled)).set("skipped", static_cast<std::int64_t>(skipped))
        .set("bisect_renders", static_cast<std::int64_t>(bisect_renders)).set("bisections", bisections);
    return v;
}

expected<PipelineState, std::string> PipelineState::from_json(const json::Value& v) {
    if (!v.is_object()) return unexpected(std::string("checkpoint is not an object"));
    PipelineState s;
    s.version = int(v["version"].as_number(0));
    if (s.version != 1) return unexpected(std::string("unknown checkpoint version"));
    s.dimension = std::size_t(v["dimension"].as_number());
    s.elapsed_s = v["elapsed_s"].as_number();
    s.evals = long(v["evals"].as_number());
    s.sessions = std::max(1, int(v["sessions"].as_number(1)));
    s.best = vec_from(v["best"]);
    s.best_loss = v["best_loss"].is_null() ? std::numeric_limits<double>::infinity() : v["best_loss"].as_number();
    for (const auto& e : v["done"].as_array()) s.done.push_back(int(e.as_number()));
    for (const auto& e : v["kept"].as_array()) s.kept.push_back(vec_from(e));
    for (const auto& e : v["finalists"].as_array()) s.finalists.push_back(vec_from(e));
    for (const auto& e : v["stages"].as_array()) {
        StageReport r;
        r.name = e["name"].as_string();
        r.free = int(e["free"].as_number());
        r.before = e["before"].as_number();
        r.after = e["after"].as_number();
        r.evals = long(e["evals"].as_number());
        r.seconds = e["seconds"].as_number();
        s.stages.push_back(std::move(r));
    }
    for (const auto& [k, n] : v["outcomes"].as_object()) s.outcomes[parse_outcome(k)] = long(n.as_number());
    s.resampled = long(v["resampled"].as_number());
    s.skipped = long(v["skipped"].as_number());
    s.bisect_renders = long(v["bisect_renders"].as_number());
    s.bisections = int(v["bisections"].as_number());
    return s;
}

expected<void, std::string> PipelineState::save(const std::filesystem::path& path) const {
    // Written beside and renamed over, so a kill in the middle of a write leaves the previous checkpoint.
    const auto tmp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return unexpected("cannot write " + tmp.string());
        f << to_json().dump(1) << "\n";
        if (!f) return unexpected("cannot write " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) return unexpected("cannot replace " + path.string() + ": " + ec.message());
    return {};
}

expected<PipelineState, std::string> PipelineState::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return unexpected("cannot read " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::parse(ss.str());
    if (!v) return unexpected(path.string() + ": " + v.error());
    return from_json(*v);
}

}  // namespace mimicry::opt
