// mimicry capture (docs/spec/06_TARGETS.md section 6, WP5.9): presets that only the plugin's own window can
// reach. The plugin's editor opens in a window of the editor worker; the user (or a taught "next preset" click)
// steps through the presets, each one is captured as a state, and a second worker verifies it exactly like a
// harvested preset. Windows only: the editor needs a window.

#include <chrono>
#include <cstdio>
#include <format>
#include <thread>

#include "app/Commands.hpp"
#include "app/Console.hpp"
#include "features/Pitch.hpp"
#include "supervisor/WorkerClient.hpp"
#include "targets/Harvest.hpp"

namespace mimicry::app {

namespace fs = std::filesystem;

int cmd_capture(const Args& a) {
    Console& con = Console::get();
    if (a.positional.empty())
        return fail("capture", "usage: mimicry capture <plugin> [--auto] [--max N] [--wait-ms MS] [--continue] [--out DIR]");
#ifndef _WIN32
    return fail("capture", "capturing from the plugin's window needs Windows (the editor is hosted in a Win32 window)");
#else
    const std::string path = a.positional[0];
    if (auto f = host::format_from_path(path); !f) return fail("capture", f.error());
    if (con.events()) {
        con.emit(json::Value().set("event", "start").set("command", "capture").set("plugin", path));
        con.emit(json::Value().set("event", "phase").set("name", "opening the plugin"));
    }
    auto editor = supervisor::WorkerClient::open(path, {}, worker_options(a));
    if (!editor) return fail("capture", editor.error().message);
    auto checker = supervisor::WorkerClient::open(path, {}, worker_options(a));
    if (!checker) return fail("capture", checker.error().message);
    const host::PluginInfo info = (*editor)->info();
    const auto params = (*editor)->parameters();
    const fs::path root = a.get("--out", "presets");
    const fs::path library = opt::plugin_file_path(root, info, "");

    int key = 48;
    if (a.has("--note")) key = features::parse_note_name(a.get("--note")).value_or(48);
    host::Job plan = note_job(a, key, 0, 600);
    plan.tail_max_ms = 2000;
    auto made = targets::PresetVerifier::create(**checker, plan);
    if (!made) return fail("capture", made.error());
    targets::PresetVerifier verifier = std::move(*made);

    targets::HarvestResult result;
    result.floor = verifier.floor();
    result.threshold = verifier.threshold();
    int ok_total = 0;
    if (a.has("--continue")) {
        if (auto prior = targets::load_library(library)) {
            for (auto& e : *prior) {
                if (e.ref.method != host::PresetMethod::Capture) continue;
                if (e.outcome == targets::PresetOutcome::Ok) {
                    if (!verifier.adopt(e)) continue;
                    ok_total = std::max(ok_total, e.number);
                }
                result.entries.push_back(std::move(e));
            }
            con.info(std::format("continuing: {} captured preset(s) kept", ok_total));
        }
    }
    const int prior_entries = static_cast<int>(result.entries.size());

    auto stored = load_profile(a, info);
    json::Value click;
    const bool want_auto = a.has("--auto");
    if (stored && stored->recipe && stored->recipe->method == host::PresetMethod::Capture && stored->recipe->click.is_object() &&
        !a.has("--no-recipe"))
        click = stored->recipe->click;
    if (want_auto && !click.is_object())
        con.warn("--auto needs a taught \"next preset\" click; teach it in the plugin window (Teach click), then Auto-capture");

    con.watch_cancel_file(library / "cancel");
    {
        std::error_code ec;
        fs::create_directories(library, ec);
        fs::remove(library / "cancel", ec);
    }
    const int max_presets = int(a.num("--max", 0));
    const double wait_ms = a.num("--wait-ms", 400);
    if (con.events()) con.emit(json::Value().set("event", "phase").set("name", "capturing"));
    auto size = (*editor)->editor_open(info.name + " - Mimicry capture", click);
    if (!size) return fail("capture", "cannot open the plugin's editor: " + size.error().message);
    con.info("The plugin's window is open: pick presets (Capture, or Teach click + Auto-capture); Done when finished.");
    if (want_auto && click.is_object()) {
        json::Value o;
        o.set("max", max_presets > 0 ? max_presets : 200).set("wait_ms", wait_ms);
        (void)(*editor)->editor_action("auto", o);
    }

    int taken = 0, ok_so_far = ok_total;
    bool cancelled = false, lost = false;
    json::Value taught;
    auto take_new = [&](int available) {
        while (taken < available) {
            std::string hash;
            auto cap = (*editor)->editor_take(taken, &hash);
            ++taken;
            if (!cap) {
                con.warn("lost a capture: " + cap.error().message);  // reported, not dropped (rule 6)
                continue;
            }
            // The plugin's own name for the preset, when its state holds one (the worker's default is capture-NNN).
            if (cap->name.rfind("capture-", 0) == 0)
                if (const std::string guessed = targets::guess_preset_name(cap->state); !guessed.empty()) cap->name = guessed;
            targets::HarvestEntry e;
            e.ref.method = host::PresetMethod::Capture;
            e.ref.index = taken;
            e.ref.name = cap->name;
            e.name = cap->name;
            e.position = taken;
            e.of = taken;
            verifier.verify(e, *cap);
            if (e.outcome == targets::PresetOutcome::Ok) {
                e.number = ++ok_total;
                ++ok_so_far;
                verifier.accept(e);
            }
            if (con.events()) {
                json::Value v;
                v.set("event", "preset").set("method", "capture").set("index", e.ref.index).set("position", e.position).set("of", e.of)
                    .set("name", e.name).set("outcome", std::string(targets::to_string(e.outcome))).set("number", e.number)
                    .set("message", e.message).set("ok", ok_so_far);
                con.emit(std::move(v));
            } else {
                con.info(std::format("  capture {:>3} {:<20} {}{}", taken, e.name, targets::to_string(e.outcome),
                                     e.message.empty() ? "" : " (" + e.message + ")"));
            }
            result.entries.push_back(std::move(e));
        }
    };
    bool asked_done = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto st = (*editor)->editor_poll();
        if (!st) {
            con.warn("the editor worker was lost: " + st.error().message);
            lost = true;
            break;
        }
        take_new(st->captures);
        if (st->click.is_object()) taught = st->click;
        for (const auto& ev : st->events) {
            if (con.events()) con.emit(json::Value().set("event", "log").set("text", ev));
        }
        if (!st->open) break;
        if (!asked_done && con.cancel_requested()) {
            cancelled = true;
            asked_done = true;
            (void)(*editor)->editor_action("done");
        } else if (!asked_done && max_presets > 0 && ok_so_far >= max_presets) {
            asked_done = true;
            (void)(*editor)->editor_action("done");
        }
    }
    if (!lost) {
        if (auto st = (*editor)->editor_poll()) take_new(st->captures);
    }
    (*editor)->editor_close();

    targets::MethodReport rep;
    rep.method = host::PresetMethod::Capture;
    rep.offered = rep.tried = taken + prior_entries;
    rep.ok = ok_so_far;
    rep.resumed = prior_entries;
    if (cancelled) rep.note = "cancelled";
    result.methods.push_back(rep);
    result.cancelled = cancelled;
    if (ok_total > 0) {
        opt::HarvestRecipe recipe;
        recipe.method = host::PresetMethod::Capture;
        recipe.count = rep.offered;
        recipe.found = ok_so_far;
        recipe.wait_ms = wait_ms;
        if (taught.is_object()) recipe.click = taught;
        result.recipe = recipe;
    }
    auto dir = targets::write_library(root, result, info, params, [&](std::uint32_t id, double u) { return (*editor)->value_text(id, u); });
    if (!dir) return fail("capture", dir.error());
    std::string out = std::format("{} presets in {}", ok_so_far, dir->string());
    if (result.recipe) {
        opt::Profile prof = stored ? *stored : opt::Profile{};
        if (!stored) {
            prof.format = info.format;
            prof.uid = info.uid;
            prof.name = info.name;
            prof.vendor = info.vendor;
            prof.version_hash = info.version_hash;
        }
        prof.recipe = result.recipe;
        const auto ppath = opt::Profile::default_path(profiles_dir(a), info);
        if (auto r = prof.save(ppath); !r) con.warn(r.error());
        else if (taught.is_object()) out += "\nthe taught click is stored in " + ppath.string() + " (next time: --auto)";
    }
    if (cancelled) out += "\ncancelled; continue with --continue";
    con.info(out);
    if (con.events()) {
        json::Value d;
        d.set("library", dir->string()).set("presets", ok_so_far).set("resumable", cancelled);
        con.done(cancelled ? "cancelled" : ok_so_far > 0 ? "finished" : "failed", std::move(d));
    }
    if (cancelled) return kExitCancelled;
    return ok_so_far > 0 ? 0 : 1;
#endif
}

}  // namespace mimicry::app
