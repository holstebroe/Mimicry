#include "app/Cli.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <ctime>
#include <format>
#include <thread>

#include "app/Console.hpp"
#include "features/Pitch.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kFlags[] = {"--no-isolation", "--no-prior", "--no-pairs", "--solve-gain", "--isolated",
                                       "--all-methods", "--raw-files", "--no-recipe", "--events", "--cancel-on-eof",
                                       "--continue", "--json", "--no-harvest"};

}  // namespace

std::string Args::get(std::string_view k, std::string fallback) const {
    auto it = opts.find(k);
    return it == opts.end() ? fallback : it->second;
}

double Args::num(std::string_view k, double fallback) const {
    auto it = opts.find(k);
    if (it == opts.end()) return fallback;
    try {
        return std::stod(it->second);
    } catch (...) {
        return fallback;
    }
}

json::Value Args::to_json() const {
    json::Object o;
    for (const auto& [k, v] : opts) o[k] = v;
    json::Array p;
    for (const auto& s : positional) p.push_back(s);
    json::Value out;
    out.set("options", json::Value(std::move(o))).set("positional", json::Value(std::move(p)));
    return out;
}

Args Args::from_json(const json::Value& v) {
    Args a;
    for (const auto& [k, val] : v["options"].as_object()) a.opts[k] = val.as_string();
    for (const auto& p : v["positional"].as_array()) a.positional.push_back(p.as_string());
    return a;
}

Args Args::merged_with(const Args& over) const {
    Args a = *this;
    for (const auto& [k, v] : over.opts) a.opts[k] = v;
    if (!over.positional.empty()) a.positional = over.positional;
    return a;
}

Args parse_args(int argc, char** argv, int first) {
    Args a;
    for (int i = first; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            const bool flag = std::find(std::begin(kFlags), std::end(kFlags), s) != std::end(kFlags);
            if (!flag && i + 1 < argc) a.opts[s] = argv[++i];
            else a.opts[s] = "1";
        } else {
            a.positional.push_back(std::move(s));
        }
    }
    return a;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

opt::RenderBatch Plugin::batch() {
    if (pool) return [this](const std::vector<host::Job>& jobs) {
        auto out = pool->render_all(jobs);
        if (test_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(test_delay_ms));
        return out;
    };
    return [this](const std::vector<host::Job>& jobs) {
        std::lock_guard lock(local_mutex);
        std::vector<expected<host::Audio, Outcome>> out;
        for (const auto& j : jobs) out.push_back(local->render(j));
        if (test_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(test_delay_ms));
        return out;
    };
}

std::string Plugin::value_text(std::uint32_t id, double u) {
    if (pool) return pool->value_text(id, u);
    std::lock_guard lock(local_mutex);
    return local->value_text(id, u);
}

supervisor::WorkerOptions worker_options(const Args& a) {
    supervisor::WorkerOptions wo;
    wo.min_timeout_s = a.num("--timeout-min", wo.min_timeout_s);
    wo.timeout_factor = a.num("--timeout-factor", wo.timeout_factor);
    return wo;
}

expected<std::unique_ptr<Plugin>, std::string> open_plugin(const std::string& path, const Args& args) {
    if (auto f = host::format_from_path(path); !f) return mimicry::unexpected(f.error());
    auto p = std::make_unique<Plugin>();
    p->path = path;
    p->test_delay_ms = int(args.num("--test-delay-ms", 0));
    if (args.has("--no-isolation")) {
        auto inst = host::open(path);
        if (!inst) return mimicry::unexpected(inst.error().message);
        p->local = std::move(*inst);
        p->info = p->local->info();
        p->params = p->local->parameters();
        return p;
    }
    const int cores = int(std::max(1u, std::thread::hardware_concurrency()));
    const int workers = int(args.num("--workers", std::max(1, cores - 1)));
    auto pool = supervisor::WorkerPool::open(path, {}, workers, worker_options(args));
    if (!pool) return mimicry::unexpected(pool.error().message);
    p->pool = std::move(*pool);
    p->info = p->pool->client(0).info();
    p->params = p->pool->client(0).parameters();
    return p;
}

fs::path profiles_dir(const Args& a) { return a.get("--profiles", "profiles"); }

std::optional<opt::Profile> load_profile(const Args& a, const host::PluginInfo& info) {
    const auto path = opt::Profile::default_path(profiles_dir(a), info);
    if (!fs::exists(path)) return std::nullopt;
    auto p = opt::Profile::load(path);
    if (!p || !p->matches(info)) {
        Console::get().warn(std::format("ignoring profile {} ({})", path.string(), p ? "different plugin build" : p.error()));
        return std::nullopt;
    }
    return std::move(*p);
}

host::Job note_job(const Args& a, int key, double cents, double gate_ms) {
    host::Job job;
    job.notes.push_back({key, cents, float(a.num("--velocity", 100.0) / 127.0), 0, a.num("--gate", gate_ms)});
    return job;
}

int fail(std::string_view command, const std::string& message) {
    Console& c = Console::get();
    if (c.events()) {
        json::Value d;
        d.set("error", message).set("command", std::string(command));
        c.done("failed", std::move(d));
    } else {
        std::fputs(std::format("mimicry {}: {}\n", command, message).c_str(), stderr);
    }
    return 1;
}

Args without_control_options(Args a) {
    for (const char* k : {"--events", "--cancel-on-eof", "--resume", "--more-minutes", "--continue", "--json"}) a.opts.erase(k);
    return a;
}

std::string timestamp_now() {
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    return stamp;
}

std::string slug(const std::string& text) {
    std::string out;
    for (char c : text) {
        const auto u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '-' || c == '_') ? c : '_';
    }
    return out.empty() ? "x" : out;
}

}  // namespace mimicry::app
