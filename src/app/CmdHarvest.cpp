// mimicry harvest (docs/spec/06_TARGETS.md section 6) with events, cancel and --continue (spec 09).

#include <cstdio>
#include <format>

#include "app/Commands.hpp"
#include "app/Console.hpp"
#include "features/Pitch.hpp"
#include "host/Codec.hpp"
#include "supervisor/WorkerClient.hpp"
#include "targets/Harvest.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

expected<HarvestSummary, std::string> harvest_to_library(host::Instance& inst, const Args& a, const fs::path& root) {
    Console& con = Console::get();
    const host::PluginInfo info = inst.info();
    const auto params = inst.parameters();

    targets::HarvestOptions ho;
    for (const auto& f : split(a.get("--from"))) ho.from.emplace_back(f);
    ho.raw_files = a.has("--raw-files");
    ho.all_methods = a.has("--all-methods");
    ho.verify_reproducible = a.has("--verify-repro");
    if (a.has("--method")) {
        ho.method = host::parse_preset_method(a.get("--method"));
        if (!ho.method) return unexpected("unknown method '" + a.get("--method") + "'");
    }
    ho.max_presets = int(a.num("--max", 0));
    ho.wait_ms = a.num("--wait-ms", ho.wait_ms);
    auto stored = load_profile(a, info);
    if (stored && stored->recipe && !a.has("--no-recipe")) {
        ho.recipe = stored->recipe;
        con.info(std::format("using the recipe in the profile: {}", host::to_string(stored->recipe->method)));
    }
    int key = 48;
    if (a.has("--note")) key = features::parse_note_name(a.get("--note")).value_or(48);
    ho.plan = note_job(a, key, 0, 600);
    if (stored && stored->settle_ms > 0) ho.plan.settle_ms = std::max(ho.plan.settle_ms, 1.5 * stored->settle_ms);
    ho.plan.settle_ms = a.num("--settle-ms", ho.plan.settle_ms);
    ho.plan.tail_max_ms = 2000;

    const fs::path library = opt::plugin_file_path(root, info, "");
    if (a.has("--continue")) {
        auto prior = targets::load_library(library);
        if (!prior) {
            con.warn("nothing to continue (" + prior.error() + "); harvesting from the start");
        } else {
            ho.prior = std::move(*prior);
            int ok = 0;
            for (const auto& e : ho.prior) ok += e.outcome == targets::PresetOutcome::Ok;
            con.info(std::format("continuing: {} preset(s) already tried, {} ok", ho.prior.size(), ok));
        }
    }
    con.watch_cancel_file(library / "cancel");
    {
        std::error_code ec;
        fs::remove(library / "cancel", ec);
    }
    ho.cancel = [] { return Console::get().cancel_requested(); };
    int ok_so_far = 0;
    for (const auto& e : ho.prior) ok_so_far += e.outcome == targets::PresetOutcome::Ok;
    ho.progress = [&](const targets::HarvestEntry& e) {
        if (e.outcome == targets::PresetOutcome::Ok) ++ok_so_far;
        if (con.events()) {
            json::Value v;
            v.set("event", "preset").set("method", std::string(host::to_string(e.ref.method))).set("index", e.ref.index).set("position", e.position)
                .set("of", e.of).set("name", e.name).set("outcome", std::string(targets::to_string(e.outcome))).set("number", e.number)
                .set("message", e.message).set("ok", ok_so_far);
            con.emit(std::move(v));
            return;
        }
        con.info(std::format("  {:<13} {:>3} {:<24} {}{}", host::to_string(e.ref.method), e.ref.index, e.name.empty() ? "-" : e.name,
                             targets::to_string(e.outcome),
                             e.outcome == targets::PresetOutcome::Ok ? std::format(" #{}", e.number)
                             : e.message.empty()                    ? std::string()
                                                                    : " (" + e.message + ")"));
    };
    if (con.events()) con.emit(json::Value().set("event", "phase").set("name", "harvesting"));

    auto res = targets::harvest(inst, ho);
    if (!res) return unexpected(res.error());
    auto dir = targets::write_library(root, *res, info, params, [&](std::uint32_t id, double u) { return inst.value_text(id, u); });
    if (!dir) return unexpected(dir.error());

    std::string out;
    if (!res->listing_error.empty()) out += "the plugin's preset listing failed: " + res->listing_error + "\n";
    for (const auto& m : res->methods)
        out += std::format("{}: {} offered, {} tried, {} ok{}\n", host::to_string(m.method), m.offered, m.tried, m.ok,
                           m.note.empty() ? "" : "; " + m.note);
    out += std::format("{} presets in {}", res->ok_count(), dir->string());
    // The recipe (WP5.10): the method that worked goes into the profile, so the next harvest starts there.
    if (res->recipe) {
        opt::Profile prof = stored ? *stored : opt::Profile{};
        if (!stored) {
            prof.format = info.format;
            prof.uid = info.uid;
            prof.name = info.name;
            prof.vendor = info.vendor;
            prof.version_hash = info.version_hash;
        }
        prof.recipe = res->recipe;
        const auto ppath = opt::Profile::default_path(profiles_dir(a), info);
        if (auto r = prof.save(ppath); !r) con.warn(r.error());
        else out += std::format("\nrecipe ({}) stored in {}", host::to_string(res->recipe->method), ppath.string());
    }
    if (res->cancelled) out += "\ncancelled; continue with --continue";
    else if (res->ok_count() == 0)
        out += "\nThe plugin offers no presets to the host (many keep them in their own browser or behind a login)."
               "\nTry: mimicry capture <plugin>  - opens the plugin's window so you can pick presets yourself"
               " (Windows), or --from <folder> for preset files.";
    con.info(out);
    return HarvestSummary{*dir, res->ok_count(), res->cancelled};
}

int cmd_harvest(const Args& a) {
    Console& con = Console::get();
    if (a.positional.empty()) return fail("harvest", "usage: mimicry harvest <plugin> [--from F] [--method M] [--all-methods] [--continue]");
    const std::string path = a.positional[0];
    if (auto f = host::format_from_path(path); !f) return fail("harvest", f.error());
    if (con.events()) {
        con.emit(json::Value().set("event", "start").set("command", "harvest").set("plugin", path));
        con.emit(json::Value().set("event", "phase").set("name", "opening the plugin"));
    }
    std::unique_ptr<host::Instance> inst;
    if (a.has("--no-isolation")) {
        auto r = host::open(path);
        if (!r) return fail("harvest", r.error().message);
        inst = std::move(*r);
    } else {
        auto r = supervisor::WorkerClient::open(path, {}, worker_options(a));
        if (!r) return fail("harvest", r.error().message);
        inst = std::move(*r);
    }
    auto sum = harvest_to_library(*inst, a, a.get("--out", "presets"));
    if (!sum) return fail("harvest", sum.error());
    if (con.events()) {
        json::Value d;
        d.set("library", sum->dir.string()).set("presets", sum->ok).set("resumable", sum->cancelled);
        con.done(sum->cancelled ? "cancelled" : sum->ok > 0 ? "finished" : "failed", std::move(d));
    }
    if (sum->cancelled) return kExitCancelled;
    return sum->ok > 0 ? 0 : 1;
}

}  // namespace mimicry::app
