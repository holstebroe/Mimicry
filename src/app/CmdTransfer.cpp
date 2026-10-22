// mimicry transfer (docs/spec/06_TARGETS.md section 2, first version): the presets of a source plugin
// matched one by one in a target plugin, any format combination. Cancel and resume work per preset and
// inside the running preset (docs/spec/09_GUI.md).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>

#include "app/Commands.hpp"
#include "app/Console.hpp"
#include "app/FitRun.hpp"
#include "app/RunDir.hpp"
#include "core/Wav.hpp"
#include "features/Pitch.hpp"
#include "output/PresetExport.hpp"
#include "supervisor/WorkerClient.hpp"
#include "targets/Harvest.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

namespace {

struct BankItem {
    int number = 0;
    std::string name;
    std::string dir;       // folder name inside the run
    std::string status = "pending";  // pending, running, done, cancelled, failed, source-failed
    double loss_db = 0, gain_db = 0, seconds = 0;
    long evals = 0;
    std::string note;
    [[nodiscard]] bool finished() const { return status == "done" || status == "failed" || status == "source-failed"; }
};

json::Value item_json(const BankItem& b) {
    json::Value v;
    v.set("number", b.number).set("name", b.name).set("dir", b.dir).set("status", b.status).set("loss_db", b.loss_db)
        .set("gain_db", b.gain_db).set("seconds", b.seconds).set("evals", static_cast<std::int64_t>(b.evals)).set("note", b.note);
    return v;
}

BankItem item_from(const json::Value& v) {
    BankItem b;
    b.number = int(v["number"].as_number());
    b.name = v["name"].as_string();
    b.dir = v["dir"].as_string();
    b.status = v["status"].as_string();
    b.loss_db = v["loss_db"].as_number();
    b.gain_db = v["gain_db"].as_number();
    b.seconds = v["seconds"].as_number();
    b.evals = long(v["evals"].as_number());
    b.note = v["note"].as_string();
    return b;
}

std::string csv(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

void write_bank(const fs::path& dir, const std::vector<BankItem>& items, double floor_db) {
    json::Array arr;
    std::string report = "number,name,status,loss_db,gain_db,evals,seconds,flags,note\n";
    for (const auto& b : items) {
        arr.push_back(item_json(b));
        // Unmatched (spec 06 section 2): a loss above 1.5 x the target's repeatability floor after the full pipeline.
        std::string flags;
        if (b.status == "done" && floor_db > 0 && b.loss_db > 1.5 * floor_db) flags = "unmatched";
        report += std::format("{},{},{},{:.4f},{:.2f},{},{:.1f},{},{}\n", b.number, csv(b.name), b.status, b.loss_db, b.gain_db, b.evals, b.seconds,
                              flags, csv(b.note));
    }
    json::Value bank;
    bank.set("floor_db", floor_db).set("presets", json::Value(std::move(arr)));
    (void)output::write_text(dir / "bank.json", bank.dump(2) + "\n");
    (void)output::write_text(dir / "bank-report.csv", report);
}

std::string dir_name(int number, const std::string& name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        out += (std::isalnum(u) || c == '-' || c == '_') ? c : '_';
    }
    if (out.size() > 40) out.resize(40);
    return std::format("{:03}-{}", number, out.empty() ? "preset" : out);
}

}  // namespace

int cmd_transfer(const Args& cli) {
    Console& con = Console::get();
    Args a = cli;
    std::optional<RunDir> run;
    bool resume = false;
    if (cli.has("--resume")) {
        auto r = RunDir::open(cli.get("--resume"));
        if (!r) return fail("transfer", r.error());
        if (r->meta()["kind"].as_string() != "transfer") return fail("transfer", cli.get("--resume") + " is not a transfer run");
        a = r->saved_args().merged_with(without_control_options(cli));
        run = std::move(*r);
        resume = true;
        std::error_code ec;
        fs::remove(run->cancel_path(), ec);
    }
    const std::string source_path = a.get("--source"), target_path = a.get("--target");
    if (source_path.empty() || target_path.empty())
        return fail("transfer", "usage: mimicry transfer --source A --target B [--library DIR] [--minutes M] [--max N]   |   --resume <run dir>");
    if (!run) {
        auto r = RunDir::create(a.get("--out", "runs"), "transfer",
                                "transfer-" + fs::path(source_path).stem().string() + "-to-" + fs::path(target_path).stem().string(),
                                without_control_options(a));
        if (!r) return fail("transfer", r.error());
        run = std::move(*r);
    }
    run->update(json::Value().set("kind", "transfer").set("plugin", target_path).set("source", source_path).set("target", target_path));
    run->set_status("running");
    con.watch_cancel_file(run->cancel_path());
    if (con.events()) {
        json::Value s;
        s.set("event", "start").set("command", "transfer").set("run_dir", run->path().string()).set("plugin", target_path)
            .set("source", source_path).set("resumed", resume);
        con.emit(std::move(s));
        con.emit(json::Value().set("event", "phase").set("name", "opening the source"));
    }
    auto finish_failed = [&](const std::string& msg) {
        run->set_status("failed", json::Value().set("message", msg));
        return fail("transfer", msg);
    };

    // The source: one worker, rendered presets only (CLAUDE.md rule 4).
    std::unique_ptr<host::Instance> source;
    if (a.has("--no-isolation")) {
        auto r = host::open(source_path);
        if (!r) return finish_failed(r.error().message);
        source = std::move(*r);
    } else {
        auto r = supervisor::WorkerClient::open(source_path, {}, worker_options(a));
        if (!r) return finish_failed(r.error().message);
        source = std::move(*r);
    }
    const host::PluginInfo sinfo = source->info();

    // The bank: from bank.json (resume), else from the source's preset library (harvested on demand).
    std::vector<BankItem> items;
    const fs::path bank_file = run->path() / "bank.json";
    double floor_db = 0;
    if (resume && fs::exists(bank_file)) {
        std::ifstream f(bank_file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (auto v = json::parse(text)) {
            for (const auto& p : (*v)["presets"].as_array()) items.push_back(item_from(p));
            floor_db = (*v)["floor_db"].as_number();
        }
    }
    if (items.empty()) {
        const fs::path root = a.get("--library-root", "presets");
        fs::path library = a.has("--library") ? fs::path(a.get("--library")) : opt::plugin_file_path(root, sinfo, "");
        if (!a.has("--library") && (a.has("--from") || a.has("--method") || !fs::exists(library / "index.json"))) {
            if (a.has("--no-harvest")) return finish_failed("no preset library for the source at " + library.string() + " (run `mimicry harvest` first)");
            if (con.events()) con.emit(json::Value().set("event", "phase").set("name", "harvesting the source presets"));
            Args ha = a;
            ha.opts.erase("--max");  // --max limits the transfer, not the library
            auto sum = harvest_to_library(*source, ha, root);
            if (!sum) return finish_failed("harvest of the source failed: " + sum.error());
            library = sum->dir;
            if (sum->cancelled) {
                run->set_status("cancelled", json::Value().set("message", "cancelled while harvesting the source"));
                con.done("cancelled", json::Value().set("run_dir", run->path().string()).set("resumable", true));
                return kExitCancelled;
            }
        }
        auto entries = targets::load_library(library);
        if (!entries) return finish_failed(entries.error());
        const auto only = split(a.get("--preset"));
        const int max_n = int(a.num("--max", 0));
        std::error_code ec;
        for (const auto& e : *entries) {
            if (e.outcome != targets::PresetOutcome::Ok) continue;
            if (!only.empty() && std::find(only.begin(), only.end(), e.name) == only.end() &&
                std::find(only.begin(), only.end(), std::to_string(e.number)) == only.end())
                continue;
            if (max_n > 0 && int(items.size()) >= max_n) break;
            BankItem b;
            b.number = e.number;
            b.name = e.name;
            b.dir = dir_name(e.number, e.name);
            fs::create_directories(run->path() / b.dir, ec);
            if (auto w = output::write_bytes(run->path() / b.dir / "source.state", e.captured.state); !w) return finish_failed(w.error());
            items.push_back(std::move(b));
        }
        if (items.empty()) return finish_failed("the source library " + library.string() + " has no usable preset");
    }
    write_bank(run->path(), items, floor_db);

    auto target = open_plugin(target_path, a);
    if (!target) return finish_failed(target.error());
    Plugin& tp = **target;
    if (const auto prof = load_profile(a, tp.info); prof && prof->probed()) floor_db = prof->floor_db;

    // The source's note plan: the same notes every preset is rendered with, and the target plays the same.
    int key = 48;
    if (a.has("--note")) {
        auto k = features::parse_note_name(a.get("--note"));
        if (!k) return finish_failed("bad --note");
        key = *k;
    }
    const double gate_ms = a.num("--gate", 800);
    host::Job sjob = note_job(a, key, 0, gate_ms);
    sjob.sample_rate = 48000;
    sjob.tail_max_ms = a.num("--tail-ms", 3000);
    if (const auto sprof = load_profile(a, sinfo); sprof && sprof->settle_ms * 1.5 > sjob.settle_ms) sjob.settle_ms = sprof->settle_ms * 1.5;
    sjob.settle_ms = a.num("--source-settle-ms", sjob.settle_ms);
    Args fit_args = a;
    fit_args.opts["--gate"] = std::to_string(gate_ms);
    fit_args.opts.erase("--note");  // the target's pitch is measured from the rendered source

    const int total = int(items.size());
    int done = 0;
    for (const auto& b : items) done += b.finished() ? 1 : 0;
    const double total_cap_s = 60.0 * a.num("--total-minutes", 0);
    double spent_s = 0;
    for (const auto& b : items) spent_s += b.seconds;
    std::string overall = "finished";
    auto emit_bank = [&] {
        if (!con.events()) return;
        con.emit(json::Value().set("event", "bank").set("presets", total).set("done", done));
    };
    emit_bank();
    run->update(json::Value().set("presets", total).set("presets_done", done));

    for (int i = 0; i < total; ++i) {
        BankItem& b = items[std::size_t(i)];
        if (b.finished()) continue;
        if (con.cancel_requested()) {
            overall = "cancelled";
            break;
        }
        if (total_cap_s > 0 && spent_s >= total_cap_s) {
            overall = "finished";
            b.note = "not started: --total-minutes used up";
            continue;
        }
        const fs::path pdir = run->path() / b.dir;
        con.info(std::format("preset {}/{}: {} ({})", i + 1, total, b.name, b.status));
        if (con.events()) {
            json::Value e;
            e.set("event", "preset_start").set("preset", i + 1).set("presets", total).set("name", b.name).set("resumed", b.status == "cancelled");
            con.emit(std::move(e));
        }
        const bool resume_this = (b.status == "cancelled" || b.status == "running") && fs::exists(pdir / "checkpoint.json");
        b.status = "running";

        // Render the source preset from its state (the target we have to match), always from scratch.
        std::vector<std::uint8_t> state;
        {
            std::ifstream sf(pdir / "source.state", std::ios::binary);
            state.assign(std::istreambuf_iterator<char>(sf), std::istreambuf_iterator<char>());
        }
        host::Job job = sjob;
        job.state = state;
        auto audio = source->render(job);
        if (!audio) {
            b.status = "source-failed";
            b.note = std::format("the source preset does not render ({})", to_string(audio.error()));
            con.warn(std::format("{}: {}", b.name, b.note));
            ++done;
            write_bank(run->path(), items, floor_db);
            emit_bank();
            continue;
        }
        {
            WavData w{audio->sample_rate, audio->channels, audio->samples};
            (void)write_wav(pdir / "source.wav", w);
        }
        features::AnalyzeOptions ao;
        ao.sample_rate = job.sample_rate;
        auto tgt = features::Target::from_mono(opt::mono_of(*audio), audio->sample_rate, ao);
        if (!tgt) {
            b.status = "source-failed";
            b.note = tgt.error();
            ++done;
            write_bank(run->path(), items, floor_db);
            emit_bank();
            continue;
        }

        FitSpec sp;
        sp.plugin = &tp;
        sp.args = &fit_args;
        sp.target = &*tgt;
        sp.analyze = ao;
        sp.dir = pdir;
        sp.target_label = sinfo.name + ": " + b.name;
        sp.default_minutes = 5;
        sp.resume = resume_this;
        sp.more_minutes = cli.num("--more-minutes", 0);
        sp.preset_index = i + 1;
        sp.preset_total = total;
        sp.preset_name = b.name;
        sp.on_progress = [&](const opt::Progress& pr) {
            run->update(json::Value().set("preset", i + 1).set("stage", pr.stage).set("loss_db", pr.best));
        };
        const FitOutcome out = run_fit(sp);
        b.seconds = out.seconds;
        b.evals = out.evals;
        if (out.have_best) {
            b.loss_db = out.loss_db;
            b.gain_db = out.gain_db;
        }
        if (out.status == "cancelled") {
            b.status = "cancelled";
            overall = "cancelled";
        } else if (out.status == "failed") {
            b.status = "failed";
            b.note = out.error;
            ++done;
        } else {
            b.status = "done";
            ++done;
        }
        spent_s = 0;
        for (const auto& x : items) spent_s += x.seconds;
        write_bank(run->path(), items, floor_db);
        run->update(json::Value().set("presets_done", done));
        if (con.events()) {
            json::Value e;
            e.set("event", "preset_done").set("preset", i + 1).set("presets", total).set("name", b.name).set("status", b.status)
                .set("loss_db", b.loss_db).set("evals", static_cast<std::int64_t>(b.evals)).set("seconds", b.seconds).set("note", b.note);
            con.emit(std::move(e));
        }
        emit_bank();
        if (overall == "cancelled") break;
    }

    // Mean loss of the matched presets, for the run list.
    double sum = 0;
    int matched = 0, unmatched = 0;
    for (const auto& b : items)
        if (b.status == "done") {
            sum += b.loss_db;
            ++matched;
            if (floor_db > 0 && b.loss_db > 1.5 * floor_db) ++unmatched;
        }
    json::Value numbers;
    numbers.set("presets", total).set("presets_done", done).set("matched", matched).set("unmatched", unmatched)
        .set("loss_db", matched ? sum / matched : 0.0).set("elapsed_s", spent_s).set("evals", static_cast<std::int64_t>(0));
    long evals = 0;
    for (const auto& b : items) evals += b.evals;
    numbers.set("evals", static_cast<std::int64_t>(evals));
    run->set_status(overall, numbers);
    write_bank(run->path(), items, floor_db);
    con.info(std::format("{}: {} of {} preset(s) done, mean loss {:.3f} dB{}; bank-report.csv in {}", overall, done, total, matched ? sum / matched : 0.0,
                         unmatched ? std::format(", {} above 1.5x the floor", unmatched) : std::string(), run->path().string()));
    if (overall == "cancelled") con.info("continue with: mimicry transfer --resume " + run->path().string());
    if (con.events()) {
        json::Value d = numbers;
        d.set("run_dir", run->path().string()).set("resumable", overall == "cancelled");
        con.done(overall, std::move(d));
    }
    return overall == "cancelled" ? kExitCancelled : 0;
}

}  // namespace mimicry::app
