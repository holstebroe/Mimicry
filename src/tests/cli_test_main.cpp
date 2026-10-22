// The command line as a front end or an agent drives it (docs/spec/09_GUI.md): --events is pure JSON lines,
// a fit and a transfer can be cancelled (`mimicry cancel`) and resumed, results exist after a cancel,
// `eval` scores a state and a VST3 target's best.vstpreset (T14).
// Usage: mimicry_cli_test <source plugin> <mimicry> [<other target plugin>].

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "core/Json.hpp"
#include "core/Wav.hpp"

namespace {

using namespace mimicry;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

json::Value read_json(const fs::path& p) {
    auto v = json::parse(read_file(p));
    return v ? *v : json::Value();
}

std::string exe;

int run_cli(const std::string& args, const fs::path& out, const fs::path& err) {
#if defined(_WIN32)
    const std::string cmd = std::format("\"\"{}\" {} > \"{}\" 2> \"{}\"\"", exe, args, out.string(), err.string());
    return std::system(cmd.c_str());
#else
    const std::string cmd = std::format("\"{}\" {} > \"{}\" 2> \"{}\"", exe, args, out.string(), err.string());
    const int st = std::system(cmd.c_str());
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

// The complete lines of an events file; a line that is not JSON fails the test (stdout is for machines).
std::vector<json::Value> read_events(const fs::path& p, bool complain = true) {
    std::vector<json::Value> out;
    std::istringstream in(read_file(p));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto v = json::parse(line);
        if (!v) {
            if (complain && !in.eof()) check(false, "events: a line is not JSON: " + line.substr(0, 80));
            continue;
        }
        out.push_back(std::move(*v));
    }
    return out;
}

// A run in the background; `wait_for` polls its events until `pred` holds, then `act` runs once.
struct Background {
    fs::path out, err;
    std::thread thread;
    int rc = -99;
    void start(const std::string& args) {
        thread = std::thread([this, args] { rc = run_cli(args, out, err); });
    }
    template <class Pred>
    bool wait_for(Pred pred, double seconds) {
        const auto t0 = Clock::now();
        while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
            for (const auto& e : read_events(out, false))
                if (pred(e)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
};

std::string run_dir_of(const std::vector<json::Value>& ev) {
    for (const auto& e : ev)
        if (e["event"].as_string() == "start") return e["run_dir"].as_string();
    return {};
}

const json::Value* last_event(const std::vector<json::Value>& ev, const std::string& name) {
    for (auto it = ev.rbegin(); it != ev.rend(); ++it)
        if ((*it)["event"].as_string() == name) return &*it;
    return nullptr;
}

// A decaying saw at 220 Hz through a one-pole low pass: something every test synth setting can approach.
void write_target(const fs::path& path) {
    const int sr = 48000;
    WavData w;
    w.sample_rate = sr;
    w.channels = 1;
    double lp = 0;
    for (int i = 0; i < sr * 6 / 10; ++i) {
        const double t = double(i) / sr;
        const double saw = 2.0 * std::fmod(220.0 * t, 1.0) - 1.0;
        lp += 0.2 * (saw - lp);
        w.samples.push_back(float(0.4 * lp * std::exp(-4.0 * t)));
    }
    check(write_wav(path, w).has_value(), "target wav written");
}

// The plugin's hazards (hangs, NaN, a crash) are planted: a profile with its safe box keeps the fits from
// spending their time in watchdog timeouts, as for any real plugin that has been probed.
void probe(const fs::path& dir, const std::string& plugin) {
    const int rc = run_cli(std::format("probe \"{}\" --no-pairs --workers 3 --profiles \"{}\" --timeout-min 1 --timeout-factor 0", plugin, (dir / "profiles").string()),
                           dir / "probe.out", dir / "probe.err");
    check(rc == 0, "probe exits 0: " + read_file(dir / "probe.err"));
}

void test_fit(const fs::path& dir, const std::string& plugin, bool vst3) {
    const fs::path wav = dir / "target.wav";
    write_target(wav);
    const std::string common = std::format("--plugin \"{}\" --wav \"{}\" --out \"{}\" --profiles \"{}\" --workers 2 --events --seed 3 --timeout-min 1 --timeout-factor 0", plugin,
                                           wav.string(), (dir / "runs").string(), (dir / "profiles").string());
    probe(dir, plugin);
    Background bg;
    bg.out = dir / "fit1.out";
    bg.err = dir / "fit1.err";
    bg.start("fit " + common + " --evals 1400 --minutes 5 --restarts 1 --progress-s 0.2 --checkpoint-s 1");
    const bool progressed = bg.wait_for([](const json::Value& e) { return e["event"].as_string() == "progress" && e["evals"].as_number() >= 200; }, 120);
    check(progressed, "fit: progress events arrive");
    auto ev = read_events(bg.out, false);
    const std::string run = run_dir_of(ev);
    check(!run.empty() && fs::exists(fs::path(run) / "run.json"), "fit: the start event names the run directory");
    check(read_json(fs::path(run) / "run.json")["status"].as_string() == "running", "fit: run.json says running while it runs");
    check(run_cli(std::format("cancel \"{}\"", run), dir / "cancel.out", dir / "cancel.err") == 0, "mimicry cancel exits 0");
    bg.join();
    ev = read_events(bg.out);
    check(bg.rc == 4, std::format("fit: a cancelled run exits 4, got {}", bg.rc));
    const auto* done = last_event(ev, "done");
    check(done && (*done)["status"].as_string() == "cancelled" && (*done)["resumable"].as_bool(), "fit: the done event says cancelled and resumable");
    check(!ev.empty() && ev.front()["event"].as_string() == "start" && &ev.back() == done, "fit: events start with start and end with done");
    double prev_evals = 0;
    bool monotonic = true;
    for (const auto& e : ev)
        if (e["event"].as_string() == "progress") {
            monotonic = monotonic && e["evals"].as_number() >= prev_evals && e["fraction"].as_number() >= 0 && e["fraction"].as_number() < 1;
            prev_evals = e["evals"].as_number();
        }
    check(monotonic && prev_evals >= 200, "fit: progress evals only grow, fraction stays below 1");
    const fs::path rd = run;
    const auto meta1 = read_json(rd / "run.json");
    check(meta1["status"].as_string() == "cancelled", "fit: run.json says cancelled");
    check(fs::exists(rd / "checkpoint.json"), "fit: a checkpoint is left");
    for (const char* f : {"best.params.json", "match.wav", "ab.wav", "diff.ppm", "report.md", "result.json"})
        check(fs::exists(rd / f), std::string("fit: results exist after a cancel: ") + f);
    check(read_file(rd / "diff.ppm").rfind("P6\n", 0) == 0, "fit: diff.ppm is a PPM");
    const double loss1 = read_json(rd / "result.json")["loss_db"].as_number(1e9);
    const long evals1 = long(read_json(rd / "result.json")["evals"].as_number());
    check(read_json(rd / "result.json")["status"].as_string() == "cancelled" && std::isfinite(loss1), "fit: result.json of the cancelled run");

    // Resume: same run directory, evaluations and time count on, never worse.
    Background bg2;
    bg2.out = dir / "fit2.out";
    bg2.err = dir / "fit2.err";
    bg2.start(std::format("fit --resume \"{}\" --events --workers 2 --progress-s 0.2", run));
    bg2.join();
    ev = read_events(bg2.out);
    check(bg2.rc == 0, std::format("fit: the resumed run exits 0, got {}: {}", bg2.rc, read_file(dir / "fit2.err")));
    done = last_event(ev, "done");
    check(done && (*done)["status"].as_string() == "finished" && (*done)["run_dir"].as_string() == run, "fit: resumed run finishes in the same directory");
    bool said_resuming = false;
    for (const auto& e : ev) said_resuming = said_resuming || e["text"].as_string().rfind("resuming", 0) == 0;
    check(said_resuming && ev.front()["resumed"].as_bool(), "fit: the resume is announced");
    const auto result = read_json(rd / "result.json");
    check(result["status"].as_string() == "finished" && result["sessions"].as_number() == 2, "fit: result.json counts two sessions");
    check(long(result["evals"].as_number()) > evals1 && result["evals"].as_number() <= 1400 + 64, std::format("fit: evaluations carry over ({} -> {})", evals1, result["evals"].as_number()));
    check(result["loss_db"].as_number(1e9) <= loss1 + 1e-6, std::format("fit: resuming never loses ground ({:.3f} -> {:.3f} dB)", loss1, result["loss_db"].as_number()));
    check(read_json(rd / "run.json")["status"].as_string() == "finished" && fs::exists(rd / "best.state"), "fit: finished run.json and best.state");
    check(!fs::exists(rd / "cancel"), "fit: the cancel request is cleared by the resume");
    check(read_file(rd / "report.md").find("resumed 1 time") != std::string::npos, "fit: report.md mentions the resume");

    // A finished run is not resumable by default (its time cap is used up only when it was the cap).
    const auto listing = dir / "runs.json";
    check(run_cli(std::format("runs --json --out \"{}\"", (dir / "runs").string()), listing, dir / "runs.err") == 0, "mimicry runs --json");
    const auto arr = read_json(listing);
    check(arr.as_array().size() == 1 && arr.as_array()[0]["status"].as_string() == "finished", "runs: one finished run");

    // eval scores the saved state (and for VST3 the .vstpreset of the same state alike, T14).
    const double fit_loss = result["loss_db"].as_number();
    auto eval_loss = [&](const fs::path& state, const std::string& tag) {
        const fs::path o = dir / ("eval-" + tag + ".json");
        const int rc = run_cli(std::format("eval --plugin \"{}\" --state \"{}\" --wav \"{}\" --profiles \"{}\" --workers 1 --json", plugin, state.string(), wav.string(),
                                           (dir / "profiles").string()),
                               o, dir / ("eval-" + tag + ".err"));
        check(rc == 0, "eval exits 0 for " + tag + ": " + read_file(dir / ("eval-" + tag + ".err")));
        return read_json(o)["loss_db"].as_number(1e9);
    };
    const double loss_state = eval_loss(rd / "best.state", "state");
    check(std::isfinite(loss_state) && loss_state < fit_loss + 3.0 && loss_state > fit_loss - 3.0,
          std::format("eval: the saved state scores near the fit ({:.3f} vs {:.3f} dB)", loss_state, fit_loss));
    if (vst3) {
        check(fs::exists(rd / "best.vstpreset"), "VST3: best.vstpreset is written");
        const double loss_preset = eval_loss(rd / "best.vstpreset", "vstpreset");
        check(std::abs(loss_preset - loss_state) < 1e-6, std::format("VST3: the .vstpreset renders like the state ({:.6f} vs {:.6f} dB)", loss_preset, loss_state));
    } else {
        check(!fs::exists(rd / "best.vstpreset"), "CLAP: no .vstpreset");
    }
}

void test_transfer(const fs::path& dir, const std::string& source, const std::string& target, const std::string& tag) {
    const std::string common = std::format("transfer --source \"{}\" --target \"{}\" --out \"{}\" --library-root \"{}\" --profiles \"{}\" --workers 2 --events --max 3 --timeout-min 1 --timeout-factor 0 "
                                           "--evals 450 --minutes 2 --restarts 1 --progress-s 0.2 --checkpoint-s 1 --seed 5",
                                           source, target, (dir / ("runs-" + tag)).string(), (dir / "lib").string(), (dir / "profiles").string());
    Background bg;
    bg.out = dir / (tag + "1.out");
    bg.err = dir / (tag + "1.err");
    bg.start(common);
    // Cancel in the middle of the second preset: the first is done, the second keeps a checkpoint.
    const bool second = bg.wait_for([](const json::Value& e) { return e["event"].as_string() == "progress" && e["preset"].as_number() == 2 && e["evals"].as_number() >= 150; }, 300);
    check(second, tag + ": the second preset is running (first harvested on demand, then fitted)");
    auto ev = read_events(bg.out, false);
    const std::string run = run_dir_of(ev);
    check(!run.empty(), tag + ": transfer start event names the run");
    check(run_cli(std::format("cancel \"{}\"", run), dir / (tag + "-cancel.out"), dir / (tag + "-cancel.err")) == 0, tag + ": cancel");
    bg.join();
    ev = read_events(bg.out);
    check(bg.rc == 4, std::format("{}: cancelled transfer exits 4, got {}", tag, bg.rc));
    const fs::path rd = run;
    auto bank = read_json(rd / "bank.json");
    check(bank["presets"].as_array().size() == 3, tag + ": three presets in the bank");
    const json::Array items = bank["presets"].as_array();  // a copy: `bank` is read again after the resume
    check(items.size() == 3 && items[0]["status"].as_string() == "done" && items[1]["status"].as_string() == "cancelled" && items[2]["status"].as_string() == "pending",
          tag + ": bank statuses after the cancel: done, cancelled, pending");
    check(items.size() == 3 && fs::exists(rd / items[1]["dir"].as_string() / "checkpoint.json"), tag + ": the running preset left a checkpoint");
    check(items.size() == 3 && fs::exists(rd / items[0]["dir"].as_string() / "best.params.json") && fs::exists(rd / items[0]["dir"].as_string() / "source.wav"),
          tag + ": the finished preset has its results and the source render");
    check(read_json(rd / "run.json")["status"].as_string() == "cancelled", tag + ": run.json cancelled");
    bool preset_done = false;
    for (const auto& e : ev) preset_done = preset_done || (e["event"].as_string() == "preset_done" && e["preset"].as_number() == 1 && e["status"].as_string() == "done");
    check(preset_done, tag + ": preset_done event for the first preset");

    Background bg2;
    bg2.out = dir / (tag + "2.out");
    bg2.err = dir / (tag + "2.err");
    bg2.start(std::format("transfer --resume \"{}\" --events --workers 2 --progress-s 0.2", run));
    bg2.join();
    ev = read_events(bg2.out);
    check(bg2.rc == 0, std::format("{}: resumed transfer exits 0, got {}: {}", tag, bg2.rc, read_file(dir / (tag + "2.err"))));
    bank = read_json(rd / "bank.json");
    int done = 0;
    for (const auto& b : bank["presets"].as_array()) done += b["status"].as_string() == "done";
    check(done == 3, std::format("{}: all three presets done after the resume ({})", tag, done));
    check(read_json(rd / "run.json")["status"].as_string() == "finished", tag + ": run.json finished");
    const std::string csv = read_file(rd / "bank-report.csv");
    check(std::count(csv.begin(), csv.end(), '\n') == 4, tag + ": bank-report.csv has a header and three rows");
    const auto* fin = last_event(ev, "done");
    check(fin && (*fin)["status"].as_string() == "finished" && (*fin)["presets_done"].as_number() == 3, tag + ": done event of the resumed transfer");
    // The cancelled preset continued from its checkpoint: its result counts two sessions.
    const auto r2 = read_json(rd / items[1]["dir"].as_string() / "result.json");
    check(r2["sessions"].as_number() == 2, tag + ": the cancelled preset was resumed inside its own run");
    // The second preset's loss: the source is the same plugin or a sibling; just require something finite.
    for (const auto& b : bank["presets"].as_array()) check(std::isfinite(b["loss_db"].as_number(1e9)) && b["loss_db"].as_number() < 60, tag + ": a finite loss for " + b["name"].as_string());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: mimicry_cli_test <plugin> <mimicry> [<other target plugin>]\n", stderr);
        return 2;
    }
    const std::string plugin = argv[1];
    exe = argv[2];
    const auto dir = fs::temp_directory_path() / ("mimicry_cli_test_" + fs::path(plugin).extension().string().substr(1) + (argc > 3 ? "_x" : ""));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const bool vst3 = fs::path(plugin).extension() == ".vst3";
    if (argc > 3) {
        probe(dir, argv[3]);
        test_transfer(dir, plugin, argv[3], "cross");
    } else {
        test_fit(dir, plugin, vst3);
        test_transfer(dir, plugin, plugin, "same");
    }
    if (failures == 0) fs::remove_all(dir);
    else std::fprintf(stderr, "kept %s\n", dir.string().c_str());
    if (failures == 0) std::puts("mimicry_cli_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
