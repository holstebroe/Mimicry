// T17 (docs/spec/07_OUTPUT_VALIDATION.md section 4): the test synth's factory bank harvested through a worker,
// per method, with the planted duplicate and no-op reported, the library written, file presets read back, the
// recipe used, and source renders from the captured state equal to renders from its parameters.
// Usage: mimicry_harvest_test <plugin.clap|plugin.vst3> <mimicry_worker> [<mimicry>].

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "core/Json.hpp"
#include "supervisor/WorkerClient.hpp"
#include "targets/Harvest.hpp"
#include "tests/test_synth/TestSynthCore.hpp"

namespace {

using namespace mimicry;
using host::PresetMethod;
using targets::PresetOutcome;
namespace ts = mimicry::testsynth;
namespace fs = std::filesystem;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

int count(const targets::HarvestResult& r, PresetOutcome o) {
    return static_cast<int>(std::ranges::count_if(r.entries, [&](const targets::HarvestEntry& e) { return e.outcome == o; }));
}

const targets::HarvestEntry* named(const targets::HarvestResult& r, const std::string& name) {
    for (const auto& e : r.entries)
        if (e.name == name) return &e;
    return nullptr;
}

// The factory bank through one method: Init no-change, Lead Copy a duplicate of Lead, four presets ok, each
// state reproducing its sound and its parameter snapshot reproducing the state's render.
void check_bank(const targets::HarvestResult& r, const std::string& m, bool named_presets) {
    check(r.ok_count() == ts::kFactoryCount - 2, std::format("{}: {} presets ok, want {}", m, r.ok_count(), ts::kFactoryCount - 2));
    check(count(r, PresetOutcome::Duplicate) == 1, m + ": one duplicate");
    check(!r.entries.empty() && r.entries.front().outcome == PresetOutcome::NoChange, m + ": the first (Init) changes nothing");
    const auto& dup = *std::ranges::find_if(r.entries, [](const auto& e) { return e.outcome == PresetOutcome::Duplicate; });
    check(dup.ref.index == 4 || dup.name == "Lead Copy", m + ": the duplicate is slot 4 (Lead Copy)");
    check(dup.duplicate_of == 2, m + ": it duplicates preset #2 (Lead): " + std::to_string(dup.duplicate_of));
    for (const auto& e : r.entries)
        if (e.outcome == PresetOutcome::Ok)
            check(e.params_reproduce && !e.captured.state.empty(), m + ": " + e.name + " renders the same from its parameters");
    if (named_presets) check(named(r, "Soft Pad") && named(r, "Vibrato"), m + ": the plugin's names are kept");
    check(r.recipe && r.recipe->method == r.entries.front().ref.method && r.recipe->found == r.ok_count(), m + ": recipe");
}

int run_cli(const std::string& exe, const std::string& args, const fs::path& out) {
#if defined(_WIN32)
    const std::string cmd = std::format("\"\"{}\" {} > \"{}\" 2>&1\"", exe, args, out.string());
    return std::system(cmd.c_str());
#else
    const std::string cmd = std::format("\"{}\" {} > \"{}\" 2>&1", exe, args, out.string());
    const int st = std::system(cmd.c_str());
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: mimicry_harvest_test <plugin> <mimicry_worker> [<mimicry>]\n", stderr);
        return 2;
    }
    const std::string plugin = argv[1];
    supervisor::WorkerOptions wo;
    wo.worker_exe = argv[2];
    auto client = supervisor::WorkerClient::open(plugin, {}, wo);
    if (!client) {
        std::fprintf(stderr, "FAIL: open: %s\n", client.error().message.c_str());
        return 1;
    }
    auto& inst = **client;
    const bool clap = inst.info().format == host::PluginFormat::Clap;
    const std::string fmt(host::to_string(inst.info().format));
    const fs::path dir = fs::temp_directory_path() / ("mimicry_harvest_test_" + fmt);
    fs::remove_all(dir);
    fs::create_directories(dir);

    targets::HarvestOptions base;
    base.standard_folders = false;

    // The bank, per method: CLAP preset discovery and MIDI program change; VST3 the program list.
    const std::vector<PresetMethod> methods = clap ? std::vector{PresetMethod::ClapPreset, PresetMethod::MidiProgram}
                                                   : std::vector{PresetMethod::ProgramList};
    std::optional<targets::HarvestResult> first;
    for (const PresetMethod m : methods) {
        auto o = base;
        o.method = m;
        auto r = targets::harvest(inst, o);
        check(r.has_value(), std::string(to_string(m)) + ": harvest runs: " + (r ? std::string() : r.error()));
        if (!r) continue;
        check_bank(*r, std::string(to_string(m)), m == PresetMethod::ClapPreset);
        if (m != PresetMethod::ClapPreset) {
            const auto& rep = r->methods.front();
            check(rep.tried < rep.offered && !rep.note.empty(), std::string(to_string(m)) + ": a blind method stops at the end of the bank");
        }
        if (!first) first = std::move(*r);
    }

    // The ladder stops at the first method that yields presets; a recipe puts its method first.
    {
        auto r = targets::harvest(inst, base);
        check(r && r->methods.size() == 1 && r->methods.front().method == methods.front(), "the ladder stops at the first method that works");
        auto o = base;
        opt::HarvestRecipe recipe;
        recipe.method = PresetMethod::ProgramParam;
        recipe.param_id = ts::kProgram;
        o.recipe = recipe;
        auto rr = targets::harvest(inst, o);
        check(rr && rr->methods.size() == 1 && rr->methods.front().method == PresetMethod::ProgramParam && rr->ok_count() == 3,
              "a recipe's method goes first (program parameter: programs 1-3 ok)");
    }

    // The library on disk, and its states read back as file presets (ladder step 1).
    if (first) {
        auto lib = targets::write_library(dir / "presets", *first, inst.info(), inst.parameters(),
                                          [&](std::uint32_t id, double u) { return inst.value_text(id, u); });
        check(lib.has_value(), "library written");
        if (lib) {
            auto index = json::parse(read_file(*lib / "index.json"));
            check(index && (*index)["presets"].as_array().size() == first->entries.size(), "index.json lists every preset");
            int states = 0;
            for (const auto& e : fs::directory_iterator(*lib)) states += e.path().extension() == ".state";
            check(states == first->ok_count(), "one state file per ok preset");
            const std::string csv = read_file(*lib / "harvest-report.csv");
            check(std::ranges::count(csv, '\n') == static_cast<long>(first->entries.size()) + 1, "harvest-report.csv has a row per preset");
            check(fs::exists(*lib / "002-Lead.state") || !clap, "file names carry the number and the plugin's name");

            auto o = base;
            o.from = {*lib};
            o.method = PresetMethod::File;
            auto files = targets::harvest(inst, o);
            check(files && files->ok_count() == first->ok_count() && count(*files, PresetOutcome::Duplicate) == 0,
                  "the library's states load back as file presets");
            if (files && !files->entries.empty())
                check(files->entries.front().captured.state == first->entries[1].captured.state, "a file preset reloads to the same state");
        }
    }

    // A source render from a harvested state through the worker (the state goes through shared memory).
    if (first && first->ok_count() > 0) {
        const auto& e = *std::ranges::find_if(first->entries, [](const auto& x) { return x.outcome == PresetOutcome::Ok; });
        host::Job job;
        job.notes.push_back({48, 0, 0.8f, 0, 400});
        job.state = e.captured.state;
        auto a = inst.render(job);
        job.state.clear();
        job.coords = e.captured.coords;
        auto b = inst.render(job);
        check(a && b && a->samples == b->samples, "worker: render from state equals render from parameters");
    }

    // The command line: library and recipe written, the recipe used by the next run.
    if (argc > 3) {
        const std::string args = std::format("harvest \"{}\" --out \"{}\" --profiles \"{}\"", plugin, (dir / "cli").string(),
                                             (dir / "profiles").string());
        check(run_cli(argv[3], args, dir / "run1.txt") == 0, "mimicry harvest exits 0: " + read_file(dir / "run1.txt"));
        check(run_cli(argv[3], args, dir / "run2.txt") == 0, "second mimicry harvest exits 0");
        const std::string second = read_file(dir / "run2.txt");
        check(second.find("using the recipe") != std::string::npos, "the second harvest uses the stored recipe");
        check(second.find("4 presets in") != std::string::npos, "the second harvest finds the bank again");
    }

    fs::remove_all(dir);
    if (failures == 0) std::puts("mimicry_harvest_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
