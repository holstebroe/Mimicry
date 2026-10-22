// The front end's logic without a window (docs/spec/09_GUI.md): forms to command lines, events to the run
// model, and with a plugin and the `mimicry` executable an end-to-end run through ChildProcess: progress
// arrives, "cancel" on stdin stops the run cleanly, the form's resume arguments continue it.
// Usage: mimicry_gui_test [<plugin> <mimicry>].

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <thread>

#include "core/Wav.hpp"
#include "gui/ChildProcess.hpp"
#include "gui/Forms.hpp"
#include "gui/RunModel.hpp"

namespace {

using namespace mimicry;
using namespace mimicry::gui;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

bool has_pair(const std::vector<std::string>& args, const std::string& a, const std::string& b) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == a && args[i + 1] == b) return true;
    return false;
}

bool has(const std::vector<std::string>& args, const std::string& a) { return std::find(args.begin(), args.end(), a) != args.end(); }

void test_words() {
    check(split_words("a b  c") == std::vector<std::string>({"a", "b", "c"}), "split_words: spaces");
    check(split_words("--only \"Cutoff, Resonance\" --x") == std::vector<std::string>({"--only", "Cutoff, Resonance", "--x"}), "split_words: quotes");
    check(split_words("\"C:\\My Plugins\\a.vst3\"") == std::vector<std::string>({"C:\\My Plugins\\a.vst3"}), "split_words: a Windows path");
    check(split_words("  ").empty() && split_words("\"\"") == std::vector<std::string>({""}), "split_words: empty");
    const auto wl = windows_command_line("C:\\Program Files\\m.exe", {"fit", "--wav", "a b.wav", "q\"x", "C:\\dir\\", "C:\\my dir\\"});
    check(wl == "\"C:\\Program Files\\m.exe\" fit --wav \"a b.wav\" \"q\\\"x\" C:\\dir\\ \"C:\\my dir\\\\\"", "windows_command_line: quoting rules: " + wl);
}

void test_forms() {
    const Form* fit = find_form("fit");
    check(fit && fit->command == "fit" && fit->resumable, "forms: fit exists");
    if (!fit) return;
    Values v;
    check(missing_fields(*fit, v).size() == 2, "forms: plugin and WAV are required");
    v["fit.plugin"] = "C:\\Plugins\\Synth.vst3";
    v["fit.wav"] = "C:\\a b\\t.wav";
    v["fit.minutes"] = "3";
    v["fit.mode"] = "guided";
    v["fit.kind"] = "";
    v["fit.extra"] = "--only Cutoff,Level --stages \"1 5\"";
    check(missing_fields(*fit, v).empty(), "forms: nothing missing");
    const auto args = build_args(*fit, v);
    check(args.front() == "fit" && has_pair(args, "--plugin", "C:\\Plugins\\Synth.vst3") && has_pair(args, "--wav", "C:\\a b\\t.wav") &&
              has_pair(args, "--minutes", "3") && has_pair(args, "--mode", "guided"),
          "forms: fit options");
    check(!has(args, "--kind") && !has(args, "--patience"), "forms: empty fields add nothing");
    check(has_pair(args, "--only", "Cutoff,Level") && has_pair(args, "--stages", "1 5"), "forms: extra options are split into words");
    check(has(args, "--events") && has(args, "--cancel-on-eof"), "forms: always machine-readable, cancel on EOF");

    const Form* harvest = find_form("harvest");
    Values h;
    h["harvest.plugin"] = "/p/s.clap";
    h["harvest.continue"] = "1";
    h["harvest.all"] = "";
    h["harvest.max"] = "20";
    const auto ha = build_args(*harvest, h);
    check(ha[0] == "harvest" && ha[1] == "/p/s.clap" && has(ha, "--continue") && !has(ha, "--all-methods") && has_pair(ha, "--max", "20"), "forms: harvest has the plugin positionally");
    const Form* capture = find_form("capture");
    check(capture != nullptr && capture->command == "capture", "forms: a Capture form exists");
    if (capture) {
        Values c;
        c["capture.plugin"] = "/p/s.vst3";
        c["capture.auto"] = "1";
        const auto ca = build_args(*capture, c);
        check(ca[0] == "capture" && ca[1] == "/p/s.vst3" && has(ca, "--auto"), "forms: capture has the plugin positionally");
    }
    check(is_harvest_like("capture") && is_harvest_like("harvest") && !is_harvest_like("fit"), "run model: capture reports presets like harvest");
    const auto hr = build_resume_args(*harvest, "", h);
    check(has(hr, "--continue") && hr[1] == "/p/s.clap", "forms: a harvest resumes with --continue");
    h["harvest.continue"] = "";
    check(has(build_resume_args(*harvest, "", h), "--continue"), "forms: --continue is added when the box is not ticked");

    const auto ra = build_resume_args(*fit, "runs/20260101-x", v, 5);
    check(ra[0] == "fit" && has_pair(ra, "--resume", "runs/20260101-x") && has_pair(ra, "--more-minutes", "5.000000") && !has(ra, "--wav"), "forms: fit resume arguments");
    const Form* tr = find_form("transfer");
    Values t{{"transfer.source", "a.clap"}, {"transfer.target", "b.vst3"}, {"transfer.noharvest", "1"}};
    const auto ta = build_args(*tr, t);
    check(ta[0] == "transfer" && has_pair(ta, "--source", "a.clap") && has_pair(ta, "--target", "b.vst3") && has(ta, "--no-harvest") && has_pair(ta, "--note", "") == false,
          "forms: transfer options");
    for (const auto& f : forms()) {
        std::vector<std::string> keys;
        for (const auto& fld : f.fields) {
            check(std::find(keys.begin(), keys.end(), fld.key) == keys.end(), "forms: duplicate key " + fld.key);
            keys.push_back(fld.key);
            check(fld.key.rfind(f.id + ".", 0) == 0, "forms: key prefix " + fld.key);
        }
    }
}

void test_settings() {
    const auto path = fs::temp_directory_path() / "mimicry_gui_settings_test.json";
    Settings s;
    s.path = path;
    s.set("fit.plugin", "C:\\x\\y.vst3");
    s.set("fit.minutes", "7");
    check(s.save(), "settings: save");
    const auto back = Settings::load(path);
    check(back.get("fit.plugin") == "C:\\x\\y.vst3" && back.get("fit.minutes") == "7" && back.get("nope", "d") == "d", "settings: round trip");
    fs::remove(path);
    check(Settings::load("/nonexistent/dir/x.json").values().empty(), "settings: a missing file is empty");
}

void test_scan() {
    const auto root = fs::temp_directory_path() / "mimicry_gui_scan_test";
    fs::remove_all(root);
    fs::create_directories(root / "Vendor" / "Deep" / "Deeper" / "Deepest");
    std::ofstream(root / "a.clap") << "x";
    std::ofstream(root / "Vendor" / "b.clap") << "x";
    fs::create_directories(root / "Vendor" / "c.vst3" / "Contents");
    std::ofstream(root / "Vendor" / "c.vst3" / "Contents" / "inner.clap") << "x";  // inside a bundle: not a plugin
    std::ofstream(root / "Vendor" / "Deep" / "Deeper" / "Deepest" / "z.clap") << "x";
    std::ofstream(root / "readme.txt") << "x";
    const auto found = scan_plugins({root});
    bool a = false, b = false, c = false, inner = false, deepest = false;
    for (const auto& p : found) {
        a = a || p.filename() == "a.clap";
        b = b || p.filename() == "b.clap";
        c = c || p.filename() == "c.vst3";
        inner = inner || p.filename() == "inner.clap";
        deepest = deepest || p.filename() == "z.clap";
    }
    check(a && b && c && !inner && !deepest, std::format("scan: plugins found (a {} b {} c {} inner {} deepest {})", a, b, c, inner, deepest));
    fs::remove_all(root);
}

std::string ev(const std::string& json) { return json; }

void test_model() {
    RunModel m;
    m.begin("fit");
    check(m.state == RunState::Starting && m.status_line() == "Starting...", "model: starting");
    m.apply_line(ev(R"({"event":"start","command":"fit","run_dir":"runs/r1","plugin":"p.clap","target":"t.wav","resumed":false})"));
    check(m.state == RunState::Running && m.run_dir == "runs/r1" && m.plugin == "p.clap", "model: start");
    m.apply_line(ev(R"({"event":"phase","name":"fitting","time_cap_s":600,"workers":3})"));
    check(m.phase == "fitting" && m.time_cap_s == 600, "model: phase");
    m.apply_line(ev(R"({"event":"preview","ab":"runs/r1/preview-ab.wav","best_db":9.5})"));
    check(m.files["ab"] == "runs/r1/preview-ab.wav" && m.previews == 1, "model: a preview event makes A/B available");
    m.apply_line(ev(R"({"event":"stage","name":"S1 seed","free":18,"finished":false,"before_db":13.2,"after_db":0,"evals":1,"seconds":0})"));
    m.apply_line(ev(R"({"event":"progress","stage":"S1 seed","evals":100,"best_db":9.5,"seconds":10,"time_cap_s":600,"fraction":0.0166,"evals_per_s":10})"));
    m.apply_line(ev(R"({"event":"progress","stage":"S1 seed","evals":300,"best_db":7.5,"seconds":30,"time_cap_s":600,"fraction":0.05,"evals_per_s":10})"));
    check(m.evals == 300 && m.best_db == 7.5 && m.history.size() == 2 && m.history.back().best_db == 7.5, "model: progress builds the loss history");
    check(std::abs(m.eta_seconds() - 570) < 1.0, std::format("model: fit ETA from the time cap ({})", m.eta_seconds()));
    check(m.status_line().find("S1 seed") == 0 && m.status_line().find("7.500 dB") != std::string::npos, "model: status line: " + m.status_line());
    m.apply_line(ev(R"({"event":"stage","name":"S1 seed","free":18,"finished":true,"before_db":13.2,"after_db":6.1,"evals":400,"seconds":40})"));
    check(m.stages.size() == 1 && m.stages[0].finished && m.stages[0].after_db == 6.1 && m.stages[0].evals == 400, "model: a stage row is updated in place");
    m.apply_line(ev(R"({"event":"stage","name":"S5 polish","free":18,"finished":false,"before_db":6.1,"after_db":0,"evals":0,"seconds":41})"));
    check(m.stages.size() == 2 && !m.stages[1].finished, "model: next stage");
    m.apply_line(ev(R"({"event":"log","level":"warn","text":"no profile"})"));
    m.apply_line("plain text from a plugin", true);
    m.apply_line("{broken json");
    check(m.log.size() == 3 && m.log[0] == "note: no profile" && m.log[1] == "! plain text from a plugin" && m.log[2] == "{broken json", "model: log lines");
    m.state = RunState::Cancelling;
    m.apply_line(ev(R"({"event":"done","status":"cancelled","run_dir":"runs/r1","resumable":true,"loss_db":5.5,"files":{"ab":"runs/r1/ab.wav"}})"));
    check(m.state == RunState::Cancelled && m.resumable && m.best_db == 5.5 && m.files["ab"] == "runs/r1/ab.wav" && !m.active(), "model: done cancelled");
    check(m.status_line().find("resumed") != std::string::npos, "model: cancelled status mentions resuming");
    m.process_exited(4);
    check(m.state == RunState::Cancelled && m.exit_code == 4, "model: an exit after done keeps the state");

    RunModel crash;
    crash.begin("fit");
    crash.apply_line(ev(R"({"event":"start","command":"fit","run_dir":"r"})"));
    crash.process_exited(139);
    check(crash.state == RunState::Failed && crash.error.find("139") != std::string::npos, "model: a process that dies without done has failed");

    RunModel bank;
    bank.begin("transfer");
    bank.apply_line(ev(R"({"event":"start","command":"transfer","run_dir":"runs/t","plugin":"b.vst3"})"));
    bank.apply_line(ev(R"({"event":"bank","presets":4,"done":1})"));
    check(bank.bank.size() == 4 && bank.bank[3].status == "pending" && bank.bank[3].index == 4, "model: bank rows");
    bank.apply_line(ev(R"({"event":"preset_start","preset":2,"presets":4,"name":"Lead","resumed":false})"));
    bank.apply_line(ev(R"({"event":"progress","stage":"S1 seed","evals":50,"best_db":4,"seconds":5,"time_cap_s":300,"fraction":0.5,"evals_per_s":10,"preset":2,"presets":4,"preset_name":"Lead"})"));
    check(bank.bank[1].status == "running" && bank.preset == 2 && std::abs(bank.overall_fraction() - (1 + 0.5) / 4) < 1e-9,
          std::format("model: bank fraction {}", bank.overall_fraction()));
    bank.apply_line(ev(R"({"event":"preset_done","preset":2,"presets":4,"name":"Lead","status":"done","loss_db":1.25,"evals":900,"seconds":120,"note":""})"));
    check(bank.bank[1].status == "done" && bank.bank[1].loss_db == 1.25, "model: preset_done fills the row");
    bank.apply_line(ev(R"({"event":"progress","stage":"S1 seed","evals":10,"best_db":9,"seconds":3,"time_cap_s":300,"fraction":0.01,"evals_per_s":10,"preset":3,"presets":4,"preset_name":"Pluck"})"));
    check(bank.history.size() == 1 && bank.stages.empty(), "model: the next preset starts a new curve");
    check(bank.status_line().find("Preset 3/4") == 0, "model: bank status line: " + bank.status_line());

    RunModel hv;
    hv.begin("harvest");
    hv.apply_line(ev(R"({"event":"preset","method":"clap-preset","index":1,"position":1,"of":4,"name":"Init","outcome":"no-change","number":0,"message":"x","ok":0})"));
    hv.apply_line(ev(R"({"event":"preset","method":"clap-preset","index":2,"position":2,"of":4,"name":"Pad","outcome":"ok","number":1,"message":"","ok":1})"));
    check(hv.harvested.size() == 2 && hv.harvest_ok == 1 && std::abs(hv.fraction - 0.5) < 1e-9 && hv.harvested[1].outcome == "ok", "model: harvest rows");
    check(format_duration(75) == "1:15" && format_duration(3725) == "1:02:05" && format_duration(-1) == "-", "format_duration");
}

// --- end to end -------------------------------------------------------------------------------

struct Session {
    ChildProcess proc;
    std::mutex mutex;
    RunModel model;
    std::atomic<bool> exited{false};
    std::string stderr_text;

    bool start(const std::string& exe, const std::vector<std::string>& args, const std::string& command) {
        model.begin(command);
        auto r = proc.start(exe, args, [this](const std::string& line, bool err) {
            std::lock_guard lock(mutex);
            if (err) stderr_text += line + "\n";
            model.apply_line(line, err);
        }, [this](int code) {
            std::lock_guard lock(mutex);
            model.process_exited(code);
            exited = true;
        });
        if (!r) std::fprintf(stderr, "start failed: %s\n", r.error().c_str());
        return bool(r);
    }
    template <class Pred>
    bool wait_for(Pred pred, double seconds) {
        const auto t0 = Clock::now();
        while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
            {
                std::lock_guard lock(mutex);
                if (pred(model)) return true;
            }
            if (exited) {
                std::lock_guard lock(mutex);
                return pred(model);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
    void finish() {
        proc.wait();
    }
};

void test_end_to_end(const std::string& plugin, const std::string& exe) {
    const auto dir = fs::temp_directory_path() / "mimicry_gui_e2e";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        WavData w;
        w.sample_rate = 48000;
        w.channels = 1;
        double lp = 0;
        for (int i = 0; i < 48000 * 6 / 10; ++i) {
            const double t = double(i) / 48000;
            lp += 0.2 * ((2.0 * std::fmod(220.0 * t, 1.0) - 1.0) - lp);
            w.samples.push_back(float(0.4 * lp * std::exp(-4.0 * t)));
        }
        (void)write_wav(dir / "t.wav", w);
    }
    const Form* probe = find_form("probe");
    const Form* fit = find_form("fit");
    Values pv{{"probe.plugin", plugin}, {"probe.nopairs", "1"}, {"probe.profiles", (dir / "profiles").string()}, {"probe.workers", "3"},
              {"probe.extra", "--timeout-min 1 --timeout-factor 0"}};
    {
        Session s;
        check(s.start(exe, build_args(*probe, pv), "probe"), "e2e: probe starts");
        check(s.wait_for([](const RunModel& m) { return m.state == RunState::Finished; }, 240), "e2e: probe finishes: " + s.stderr_text);
        s.finish();
        check(s.model.exit_code == 0 && fs::exists(dir / "profiles"), "e2e: probe wrote the profile");
    }
    Values fv{{"fit.plugin", plugin}, {"fit.wav", (dir / "t.wav").string()}, {"fit.minutes", "5"}, {"fit.evals", "1000"}, {"fit.restarts", "1"},
              {"fit.out", (dir / "runs").string()}, {"fit.profiles", (dir / "profiles").string()}, {"fit.workers", "2"},
              {"fit.extra", "--progress-s 0.2 --checkpoint-s 1 --timeout-min 1 --timeout-factor 0"}};
    Session s;
    check(s.start(exe, build_args(*fit, fv), "fit"), "e2e: fit starts");
    check(s.wait_for([](const RunModel& m) { return m.evals >= 150 && m.history.size() >= 2; }, 240), "e2e: progress reaches the model");
    {
        std::lock_guard lock(s.mutex);
        check(!s.model.run_dir.empty() && s.model.time_cap_s == 300 && s.model.state == RunState::Running, "e2e: run dir and time cap are known while running");
        check(!s.model.stages.empty(), "e2e: stage rows exist");
        check(s.model.overall_fraction() > 0 && s.model.overall_fraction() < 1, "e2e: a fraction below 1 while running");
        s.model.state = RunState::Cancelling;
    }
    s.proc.send_line("cancel");
    check(s.wait_for([](const RunModel& m) { return m.state == RunState::Cancelled; }, 120), "e2e: cancel through stdin ends in Cancelled");
    s.finish();
    std::string run;
    double loss1 = 0;
    long evals1 = 0;
    {
        std::lock_guard lock(s.mutex);
        run = s.model.run_dir;
        loss1 = s.model.best_db;
        evals1 = s.model.evals;
        check(s.model.resumable && s.model.exit_code == 4 && fs::exists(fs::path(run) / "checkpoint.json") && fs::exists(fs::path(run) / "best.params.json"),
              "e2e: cancelled run is resumable, has a checkpoint and results, exit code 4");
        check(s.model.files.count("ab") && fs::exists(s.model.files["ab"]), "e2e: the done event names ab.wav");
    }
    Session r;
    check(r.start(exe, build_resume_args(*fit, run, fv), "fit"), "e2e: resume starts");
    check(r.wait_for([](const RunModel& m) { return m.state == RunState::Finished; }, 240), "e2e: the resumed run finishes: " + r.stderr_text);
    r.finish();
    {
        std::lock_guard lock(r.mutex);
        check(r.model.resumed && r.model.run_dir == run && r.model.exit_code == 0, "e2e: resumed in the same directory");
        check(r.model.best_db <= loss1 + 1e-6 && r.model.evals >= evals1, std::format("e2e: resume continues (loss {:.3f} -> {:.3f}, evals {} -> {})", loss1, r.model.best_db, evals1, r.model.evals));
    }
    // A front end that dies: the end of its input cancels the run (--cancel-on-eof).
    Values dv = fv;
    dv["fit.evals"] = "100000";
    dv["fit.minutes"] = "10";
    Session d;
    check(d.start(exe, build_args(*fit, dv), "fit"), "e2e: second fit starts");
    check(d.wait_for([](const RunModel& m) { return m.evals >= 100; }, 240), "e2e: second fit progresses");
    d.proc.close_stdin();
    check(d.wait_for([](const RunModel& m) { return m.state == RunState::Cancelled; }, 120), "e2e: closing stdin cancels the run");
    d.finish();
    fs::remove_all(dir);
}

}  // namespace

int main(int argc, char** argv) {
    test_words();
    test_forms();
    test_settings();
    test_scan();
    test_model();
    if (argc > 2) test_end_to_end(argv[1], argv[2]);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("mimicry_gui_test: all passed");
    return 0;
}
