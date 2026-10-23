#include "app/RunDir.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "app/Console.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

expected<RunDir, std::string> RunDir::create(const fs::path& root, const std::string& kind, const std::string& name, const Args& args) {
    RunDir r;
    r.dir_ = root / (timestamp_now() + "-" + slug(name));
    for (int n = 2; fs::exists(r.dir_); ++n) r.dir_ = root / (timestamp_now() + "-" + slug(name) + "-" + std::to_string(n));
    std::error_code ec;
    fs::create_directories(r.dir_, ec);
    if (ec) return unexpected("cannot create " + r.dir_.string());
    r.meta_.set("kind", kind).set("name", name).set("created", timestamp_now()).set("args", args.to_json());
    r.set_status("running");
    return r;
}

expected<RunDir, std::string> RunDir::open(const fs::path& dir) {
    std::ifstream f(dir / "run.json", std::ios::binary);
    if (!f) return unexpected(dir.string() + " is not a run directory (no run.json)");
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::parse(ss.str());
    if (!v) return unexpected((dir / "run.json").string() + ": " + v.error());
    RunDir r;
    r.dir_ = dir;
    r.meta_ = std::move(*v);
    return r;
}

void RunDir::write() {
    meta_.set("updated", timestamp_now());
    const auto tmp = dir_ / "run.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << meta_.dump(2) << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, dir_ / "run.json", ec);
    if (ec) Console::get().warn("cannot update " + (dir_ / "run.json").string() + ": " + ec.message());
}

void RunDir::update(const json::Value& extra) {
    for (const auto& [k, v] : extra.as_object()) meta_.set(k, v);
    write();
}

void RunDir::set_status(const std::string& status, const json::Value& extra) {
    meta_.set("status", status);
    update(extra);
}

std::vector<RunInfo> list_runs(const fs::path& root) {
    std::vector<RunInfo> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory()) continue;
        auto r = RunDir::open(e.path());
        if (!r) continue;
        const auto& m = r->meta();
        RunInfo i;
        i.dir = e.path();
        i.kind = m["kind"].as_string();
        i.status = m["status"].as_string();
        i.plugin = m["plugin"].as_string();
        i.target = m["target"].as_string();
        i.updated = m["updated"].as_string();
        i.loss_db = m["loss_db"].as_number(0);
        i.elapsed_s = m["elapsed_s"].as_number(0);
        i.time_cap_s = m["time_cap_s"].as_number(0);
        i.evals = long(m["evals"].as_number(0));
        i.resumable = i.status != "finished" && (fs::exists(e.path() / "checkpoint.json") || i.kind == "transfer");
        i.meta = m;
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const RunInfo& a, const RunInfo& b) { return a.dir.filename() > b.dir.filename(); });
    return out;
}

}  // namespace mimicry::app
