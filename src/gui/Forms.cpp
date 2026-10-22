#include "gui/Forms.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mimicry::gui {

namespace fs = std::filesystem;

namespace {

Field plugin(std::string key, std::string label, std::string option, std::string hint = {}) {
    Field f;
    f.key = std::move(key);
    f.label = std::move(label);
    f.kind = FieldKind::PluginFile;
    f.option = std::move(option);
    f.hint = std::move(hint);
    f.required = true;
    return f;
}

Field text(std::string key, std::string label, std::string option, std::string def = {}, std::string hint = {}, bool advanced = false) {
    Field f;
    f.key = std::move(key);
    f.label = std::move(label);
    f.option = std::move(option);
    f.def = std::move(def);
    f.hint = std::move(hint);
    f.advanced = advanced;
    return f;
}

Field number(std::string key, std::string label, std::string option, std::string def = {}, std::string hint = {}, bool advanced = false) {
    Field f = text(std::move(key), std::move(label), std::move(option), std::move(def), std::move(hint), advanced);
    f.kind = FieldKind::Number;
    return f;
}

Field check(std::string key, std::string label, std::string option, bool on = false, std::string hint = {}, bool advanced = false) {
    Field f = text(std::move(key), std::move(label), std::move(option), on ? "1" : "", std::move(hint), advanced);
    f.kind = FieldKind::Check;
    return f;
}

Field folder(std::string key, std::string label, std::string option, std::string def = {}, std::string hint = {}, bool advanced = false) {
    Field f = text(std::move(key), std::move(label), std::move(option), std::move(def), std::move(hint), advanced);
    f.kind = FieldKind::Folder;
    return f;
}

Field choice(std::string key, std::string label, std::string option, std::vector<std::string> choices, std::string def, std::string hint = {},
             bool advanced = false) {
    Field f = text(std::move(key), std::move(label), std::move(option), std::move(def), std::move(hint), advanced);
    f.kind = FieldKind::Choice;
    f.choices = std::move(choices);
    return f;
}

Field extra(std::string key) {
    return text(std::move(key), "Extra options", "", "", "Any other command line options, e.g. --only Cutoff,Resonance --stages 1,5,7", true);
}

std::vector<Form> make_forms() {
    std::vector<Form> out;

    Form fit;
    fit.id = "fit";
    fit.title = "Fit a WAV";
    fit.command = "fit";
    fit.resumable = true;
    fit.blurb = "Find the preset of one plugin that sounds like a recording.";
    {
        Field wav;
        wav.key = "fit.wav";
        wav.label = "Target WAV";
        wav.kind = FieldKind::WavFile;
        wav.option = "--wav";
        wav.required = true;
        fit.fields = {plugin("fit.plugin", "Plugin", "--plugin", "The instrument to program (.clap or .vst3)."),
                      wav,
                      number("fit.minutes", "Minutes", "--minutes", "20", "Time cap. A run can be cancelled and resumed later."),
                      number("fit.patience", "Patience (s)", "--patience", "240", "Stop a search that has not improved for this long.", true),
                      number("fit.evals", "Evaluation budget", "--evals", "", "Stop after this many renders (empty: no limit).", true),
                      choice("fit.mode", "Mode", "--mode", {"hybrid", "guided", "unsupervised"}, "hybrid", "hybrid: names set the plan, measurements override.", true),
                      text("fit.note", "Note", "--note", "", "Target pitch override, e.g. C3. Empty: measured from the WAV.", true),
                      choice("fit.kind", "Sample kind", "--kind", {"", "note", "hit", "texture", "loop"}, "", "Empty: detected.", true),
                      number("fit.restarts", "Restarts", "--restarts", "3", "Independent polish runs.", true),
                      number("fit.topk", "Alternatives", "--top-k", "3", "Diverse answers kept next to the best.", true),
                      number("fit.seed", "Seed", "--seed", "1", "", true),
                      number("fit.workers", "Workers", "--workers", "", "Plugin processes (empty: cores - 1).", true),
                      folder("fit.out", "Runs folder", "--out", "runs", "", true),
                      folder("fit.profiles", "Profiles folder", "--profiles", "profiles", "", true),
                      extra("fit.extra")};
    }
    out.push_back(fit);

    Form transfer;
    transfer.id = "transfer";
    transfer.title = "Transfer presets";
    transfer.command = "transfer";
    transfer.resumable = true;
    transfer.blurb = "Match the presets of a source plugin in a target plugin (any mix of CLAP and VST3).";
    transfer.fields = {plugin("transfer.source", "Source plugin", "--source", "The plugin whose presets are the targets."),
                       plugin("transfer.target", "Target plugin", "--target", "The plugin that has to reproduce them."),
                       number("transfer.minutes", "Minutes per preset", "--minutes", "5", "A bank of 100 presets at 5 minutes takes about 8 hours; cancel and resume at will."),
                       number("transfer.max", "First N presets", "--max", "", "Empty: the whole bank."),
                       text("transfer.preset", "Only presets", "--preset", "", "Names or numbers, comma separated.", true),
                       folder("transfer.library", "Preset library", "--library", "", "A folder made by Harvest. Empty: the standard place, harvested on demand.", true),
                       text("transfer.note", "Note", "--note", "C3", "The note both plugins play.", true),
                       number("transfer.gate", "Gate (ms)", "--gate", "800", "", true),
                       number("transfer.velocity", "Velocity", "--velocity", "100", "", true),
                       number("transfer.total", "Total minutes", "--total-minutes", "", "Stop starting new presets after this long.", true),
                       number("transfer.evals", "Evaluations per preset", "--evals", "", "", true),
                       number("transfer.workers", "Workers", "--workers", "", "", true),
                       folder("transfer.out", "Runs folder", "--out", "runs", "", true),
                       folder("transfer.profiles", "Profiles folder", "--profiles", "profiles", "", true),
                       check("transfer.noharvest", "Do not harvest", "--no-harvest", false, "Fail instead of harvesting a missing library.", true),
                       extra("transfer.extra")};
    out.push_back(transfer);

    Form harvest;
    harvest.id = "harvest";
    harvest.title = "Harvest presets";
    harvest.command = "harvest";
    harvest.resumable = false;
    harvest.blurb = "Collect a plugin's presets as verified states (files, the plugin's catalogue, program lists, MIDI program change).";
    {
        Field p = plugin("harvest.plugin", "Plugin", {}, "The plugin whose presets are collected.");
        harvest.fields = {p,
                          text("harvest.from", "Preset files / folders", "--from", "", "Comma separated. .vstpreset and .state files, or folders of them."),
                          choice("harvest.method", "Method", "--method", {"", "file", "clap-preset", "program-data", "program-list", "midi-program", "program-param"}, "",
                                 "Empty: the ladder, stopping at the first method that yields presets.", true),
                          check("harvest.all", "Try every method", "--all-methods", false, "", true),
                          check("harvest.continue", "Continue an earlier harvest", "--continue", false, "Skip what a cancelled harvest already tried.", false),
                          number("harvest.max", "Stop after N presets", "--max", "", "", true),
                          number("harvest.wait", "Wait after each load (ms)", "--wait-ms", "200", "", true),
                          check("harvest.raw", "Try any file as a state", "--raw-files", false, "Only works where the file is the plugin's state.", true),
                          folder("harvest.out", "Library folder", "--out", "presets", "", true),
                          folder("harvest.profiles", "Profiles folder", "--profiles", "profiles", "", true),
                          extra("harvest.extra")};
    }
    out.push_back(harvest);

    Form probe;
    probe.id = "probe";
    probe.title = "Probe";
    probe.command = "probe";
    probe.blurb = "Measure what each parameter does and which settings are safe; makes later fits faster and safer.";
    probe.fields = {plugin("probe.plugin", "Plugin", {}, "The plugin to probe."),
                    text("probe.note", "Note", "--note", "C3", "", true),
                    check("probe.nopairs", "Skip pair probes", "--no-pairs", false, "", true),
                    number("probe.workers", "Workers", "--workers", "", "", true),
                    folder("probe.profiles", "Profiles folder", "--profiles", "profiles", "", true),
                    extra("probe.extra")};
    out.push_back(probe);

    Form fingerprint;
    fingerprint.id = "fingerprint";
    fingerprint.title = "Fingerprint";
    fingerprint.command = "fingerprint";
    fingerprint.blurb = "Render random presets once so later fits start from the nearest known sound.";
    fingerprint.fields = {plugin("fingerprint.plugin", "Plugin", {}, "The plugin to fingerprint."),
                          number("fingerprint.count", "Presets to render", "--count", "2000"),
                          number("fingerprint.workers", "Workers", "--workers", "", "", true),
                          folder("fingerprint.profiles", "Profiles folder", "--profiles", "profiles", "", true),
                          extra("fingerprint.extra")};
    out.push_back(fingerprint);

    Form eval;
    eval.id = "eval";
    eval.title = "Score a preset";
    eval.command = "eval";
    eval.blurb = "How close is a saved preset (best.state or .vstpreset) to a recording? No fitting.";
    {
        Field state;
        state.key = "eval.state";
        state.label = "Preset";
        state.kind = FieldKind::StateFile;
        state.option = "--state";
        state.required = true;
        Field wav;
        wav.key = "eval.wav";
        wav.label = "Target WAV";
        wav.kind = FieldKind::WavFile;
        wav.option = "--wav";
        wav.required = true;
        eval.fields = {plugin("eval.plugin", "Plugin", "--plugin"), state, wav, folder("eval.profiles", "Profiles folder", "--profiles", "profiles", "", true), extra("eval.extra")};
    }
    out.push_back(eval);
    return out;
}

// Reads an environment variable; empty when unset. _dupenv_s avoids the MSVC getenv warning.
std::string env_value(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

}  // namespace

const std::vector<Form>& forms() {
    static const std::vector<Form> f = make_forms();
    return f;
}

const Form* find_form(const std::string& id) {
    for (const auto& f : forms())
        if (f.id == id) return &f;
    return nullptr;
}

std::vector<std::string> missing_fields(const Form& form, const Values& values) {
    std::vector<std::string> out;
    for (const auto& f : form.fields) {
        if (!f.required) continue;
        const auto it = values.find(f.key);
        if (it == values.end() || it->second.empty()) out.push_back(f.label);
    }
    return out;
}

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false, have = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
            cur += '"';
            have = true;
            ++i;
        } else if (c == '"') {
            in_quotes = !in_quotes;
            have = true;
        } else if ((c == ' ' || c == '\t' || c == '\n') && !in_quotes) {
            if (have) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have) out.push_back(cur);
    return out;
}

std::vector<std::string> build_args(const Form& form, const Values& values) {
    std::vector<std::string> args{form.command};
    std::vector<std::string> positional, options, extra_words;
    for (const auto& f : form.fields) {
        const auto it = values.find(f.key);
        const std::string v = it == values.end() ? std::string() : it->second;
        if (f.kind == FieldKind::Check) {
            if (!v.empty() && v != "0" && !f.option.empty()) options.push_back(f.option);
            continue;
        }
        if (v.empty()) continue;
        if (f.option.empty()) {
            if (f.key.size() >= 6 && f.key.compare(f.key.size() - 6, 6, ".extra") == 0) {
                const auto words = split_words(v);
                extra_words.insert(extra_words.end(), words.begin(), words.end());
            } else {
                positional.push_back(v);
            }
        } else {
            options.push_back(f.option);
            options.push_back(v);
        }
    }
    args.insert(args.end(), positional.begin(), positional.end());
    args.insert(args.end(), options.begin(), options.end());
    args.insert(args.end(), extra_words.begin(), extra_words.end());
    args.push_back("--events");
    args.push_back("--cancel-on-eof");
    return args;
}

std::vector<std::string> build_resume_args(const Form& form, const std::string& run_dir, const Values& values, double extra_minutes) {
    std::vector<std::string> args{form.command};
    if (form.resumable) {
        args.push_back("--resume");
        args.push_back(run_dir);
        if (extra_minutes > 0) {
            args.push_back("--more-minutes");
            args.push_back(std::to_string(extra_minutes));
        }
        // The runs and profile folders may differ from the saved ones only through the saved options: nothing else.
        const auto w = values.find(form.id + ".workers");
        if (w != values.end() && !w->second.empty()) {
            args.push_back("--workers");
            args.push_back(w->second);
        }
    } else {
        // Harvest: the same form again, with --continue.
        args = build_args(form, values);
        if (std::find(args.begin(), args.end(), "--continue") == args.end()) args.insert(args.begin() + 1, "--continue");
        return args;
    }
    args.push_back("--events");
    args.push_back("--cancel-on-eof");
    return args;
}

std::vector<fs::path> scan_plugins(const std::vector<fs::path>& extra_roots) {
    std::vector<fs::path> roots = extra_roots;
    auto env = [](const char* n) { return env_value(n); };
#if defined(_WIN32)
    for (const char* n : {"COMMONPROGRAMFILES", "COMMONPROGRAMFILES(X86)"}) {
        if (env(n).empty()) continue;
        roots.push_back(fs::path(env(n)) / "VST3");
        roots.push_back(fs::path(env(n)) / "CLAP");
    }
    if (!env("LOCALAPPDATA").empty()) {
        roots.push_back(fs::path(env("LOCALAPPDATA")) / "Programs" / "Common" / "VST3");
        roots.push_back(fs::path(env("LOCALAPPDATA")) / "Programs" / "Common" / "CLAP");
    }
    if (!env("CLAP_PATH").empty()) {
        std::stringstream ss(env("CLAP_PATH"));
        std::string p;
        while (std::getline(ss, p, ';'))
            if (!p.empty()) roots.emplace_back(p);
    }
#elif defined(__APPLE__)
    roots.emplace_back("/Library/Audio/Plug-Ins/VST3");
    roots.emplace_back("/Library/Audio/Plug-Ins/CLAP");
    if (!env("HOME").empty()) {
        roots.push_back(fs::path(env("HOME")) / "Library/Audio/Plug-Ins/VST3");
        roots.push_back(fs::path(env("HOME")) / "Library/Audio/Plug-Ins/CLAP");
    }
#else
    roots.emplace_back("/usr/lib/vst3");
    roots.emplace_back("/usr/local/lib/vst3");
    roots.emplace_back("/usr/lib/clap");
    roots.emplace_back("/usr/local/lib/clap");
    if (!env("HOME").empty()) {
        roots.push_back(fs::path(env("HOME")) / ".vst3");
        roots.push_back(fs::path(env("HOME")) / ".clap");
    }
#endif
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            const auto ext = it->path().extension().string();
            if (ext == ".clap" || ext == ".vst3") {
                found.push_back(it->path());
                it.disable_recursion_pending();  // a .vst3 bundle is a folder: do not look inside
            }
        }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

fs::path Settings::default_path() {
    auto env = [](const char* n) { return env_value(n); };
#if defined(_WIN32)
    if (!env("APPDATA").empty()) return fs::path(env("APPDATA")) / "Mimicry" / "gui.json";
#elif defined(__APPLE__)
    if (!env("HOME").empty()) return fs::path(env("HOME")) / "Library/Application Support/Mimicry/gui.json";
#else
    if (!env("XDG_CONFIG_HOME").empty()) return fs::path(env("XDG_CONFIG_HOME")) / "mimicry" / "gui.json";
    if (!env("HOME").empty()) return fs::path(env("HOME")) / ".config/mimicry/gui.json";
#endif
    return "mimicry-gui.json";
}

Settings Settings::load(const fs::path& path) {
    Settings s;
    s.path = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) return s;
    std::stringstream ss;
    ss << f.rdbuf();
    if (auto v = json::parse(ss.str()))
        for (const auto& [k, val] : v->as_object())
            if (val.is_string()) s.values_[k] = val.as_string();
    return s;
}

bool Settings::save() const {
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    json::Object o;
    for (const auto& [k, v] : values_) o[k] = v;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << json::Value(std::move(o)).dump(2) << "\n";
    return bool(f);
}

std::string Settings::get(const std::string& key, const std::string& fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

}  // namespace mimicry::gui
