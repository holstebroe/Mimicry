// mimicry tones (docs/spec/06_TARGETS.md section 6a, WP5.13): the consensus tone of each harvested preset.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>

#include "app/Commands.hpp"
#include "features/Pitch.hpp"
#include "supervisor/WorkerClient.hpp"
#include "targets/Tones.hpp"

namespace mimicry::app {

// docs/spec/06_TARGETS.md section 6a. One worker, presets one after the other.
int cmd_tones(const Args& a) {
    if (a.positional.empty() || !a.has("--presets"))
        return fail("tones", "usage: mimicry tones <plugin> --presets DIR [--notes C2,C3] [--velocities 50,110] [--editor]");
    const std::string path = a.positional[0];
    if (auto f = host::format_from_path(path); !f) return fail("tones", f.error());
    auto library = targets::list_library(a.get("--presets"));
    if (!library) return fail("tones", library.error());

    std::vector<int> keys, velocities;
    for (const auto& n : split(a.get("--notes", "C2,C3,C4"))) {
        const auto k = features::parse_note_name(n);
        if (!k) return fail("tones", "bad note '" + n + "'");
        keys.push_back(*k);
    }
    for (const auto& v : split(a.get("--velocities", "50,110"))) {
        const int x = std::atoi(v.c_str());
        if (x < 1 || x > 127) return fail("tones", "velocity '" + v + "' is not 1..127");
        velocities.push_back(x);
    }
    if (keys.empty() || velocities.empty()) return fail("tones", "need at least one note and one velocity");
    const auto filters = split(a.get("--preset"));
    std::erase_if(*library, [&](const targets::LibraryPreset& p) {
        if (filters.empty()) return false;
        return std::ranges::none_of(filters, [&](const std::string& f) { return p.stem.find(f) != std::string::npos; });
    });
    if (library->empty()) return fail("tones", "no preset matches --preset");

    std::unique_ptr<host::Instance> inst;
    if (a.has("--no-isolation")) {
        auto r = host::open(path);
        if (!r) return fail("tones", r.error().message);
        inst = std::move(*r);
    } else {
        // A plugin signed in through its window keeps the session in the worker's memory: no recycling then.
        supervisor::WorkerOptions wo = worker_options(a);
        if (a.has("--editor")) wo.recycle_after = std::numeric_limits<int>::max();
        auto r = supervisor::WorkerClient::open(path, {}, wo);
        if (!r) return fail("tones", r.error().message);
        inst = std::move(*r);
    }
    const host::PluginInfo info = inst->info();
    std::fputs(std::format("{} ({}): {} preset(s) x {} note(s) x {} velocity(ies) x {} take(s)\n", info.name, host::to_string(info.format),
                           library->size(), keys.size(), velocities.size(), int(a.num("--takes", 5)))
                   .c_str(),
               stdout);

    // The plugin's own window (login, licence), then the user starts the run (spec 06 section 6a).
    if (a.has("--editor")) {
        if (auto e = inst->open_editor(); !e) return fail("tones", e.error().message);
        std::fputs("The plugin window is open: sign in or dismiss any dialog, then press Enter here to start.\n", stdout);
    } else if (!a.has("--start")) {
        std::fputs("Press Enter to start (or pass --start).\n", stdout);
    }
    if (a.has("--editor") || !a.has("--start")) {
        std::string line;
        std::getline(std::cin, line);
    }
    if (a.has("--editor")) inst->close_editor();

    targets::ToneRun run;
    run.presets = std::move(*library);
    run.keys = keys;
    run.velocities = velocities;
    run.tone.takes = int(a.num("--takes", 5));
    run.tone.gate_ms = a.num("--gate", run.tone.gate_ms);
    run.tone.end_db = a.num("--end-db", run.tone.end_db);
    run.tone.max_tail_ms = a.num("--max-tail-ms", run.tone.max_tail_ms);
    run.tone.max_spread = a.num("--max-spread", run.tone.max_spread);
    run.tone.settle_ms = a.num("--settle-ms", run.tone.settle_ms);
    run.out = a.get("--out", "tones");
    run.keep_takes = a.has("--keep-takes");
    run.plugin_name = info.name;
    run.plugin_uid = info.uid;
    run.plugin_version_hash = info.version_hash;
    run.plugin_format = std::string(host::to_string(info.format));

    auto res = targets::run_tones(*inst, run, [](const targets::ToneRunEntry& e) {
        std::fputs(std::format("  {:<28} {:<8} {}{}\n", e.preset, e.stem, e.status,
                               e.chosen > 0 ? std::format("  take {} of {}, spread {:.2f}", e.chosen, e.takes, e.spread) : "")
                       .c_str(),
                   stdout);
    });
    if (!res) return fail("tones", res.error());
    std::fputs(std::format("{} tone(s) written, {} unstable, {} failed; manifest {}\n", res->written, res->unstable, res->failed,
                           res->manifest.string())
                   .c_str(),
               stdout);
    return res->written > 0 ? 0 : 1;
}

}  // namespace mimicry::app
