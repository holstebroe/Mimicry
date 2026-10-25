#pragma once
// The forms of the front end as data (docs/spec/09_GUI.md section 5): which command a tab runs and which field
// becomes which option. The window code builds its controls from this table; `build_args` turns the values
// back into the command line. Portable and tested without a window.

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/Json.hpp"

namespace mimicry::gui {

enum class FieldKind {
    PluginFile,  // a .clap / .vst3 (editable list of the plugins found in the standard folders)
    WavFile,
    StateFile,   // best.state / .vstpreset
    Folder,
    Text,
    Number,
    Check,
    Choice,      // `choices`
};

struct Field {
    std::string key;     // settings key, unique per form
    std::string label;
    FieldKind kind = FieldKind::Text;
    std::string option;  // "--wav"; empty: positional (in field order, right after the command)
    std::string def;     // default value; for Check "1" or ""
    std::vector<std::string> choices;
    std::string hint;    // tooltip / help line
    bool required = false;
    bool advanced = false;  // shown under "more options"
};

struct Form {
    std::string id;       // tab id: fit, transfer, harvest, probe, eval, ...
    std::string title;
    std::string command;  // mimicry command
    std::vector<Field> fields;
    std::string blurb;
    bool can_cancel = true;
    bool resumable = false;  // the run directory can be resumed (fit, transfer); harvest resumes with --continue
};

[[nodiscard]] const std::vector<Form>& forms();
[[nodiscard]] const Form* find_form(const std::string& id);

using Values = std::map<std::string, std::string>;  // field key -> text ("1" / "" for checks)

// Fields whose required value is missing, as labels (empty: the form can start).
[[nodiscard]] std::vector<std::string> missing_fields(const Form& form, const Values& values);

// `mimicry` arguments for the form (without the executable): the command, the options of the filled fields,
// `--events --cancel-on-eof`, and the words of the free "extra options" text.
[[nodiscard]] std::vector<std::string> build_args(const Form& form, const Values& values);

// Arguments that resume a run directory or continue a harvest; `extra_minutes` > 0 adds --more-minutes.
[[nodiscard]] std::vector<std::string> build_resume_args(const Form& form, const std::string& run_dir, const Values& values, double extra_minutes = 0);

// Shell-like words: whitespace separates, double quotes group, backslash escapes a quote.
[[nodiscard]] std::vector<std::string> split_words(const std::string& text);

// Plugins found in the standard CLAP and VST3 folders of the platform (one level of vendor folders deep).
[[nodiscard]] std::vector<std::filesystem::path> scan_plugins(const std::vector<std::filesystem::path>& extra_roots = {});

// Remembered values (last used paths and options), a JSON file under the user's settings folder.
class Settings {
public:
    [[nodiscard]] static std::filesystem::path default_path();
    [[nodiscard]] static Settings load(const std::filesystem::path& path);
    bool save() const;
    [[nodiscard]] std::string get(const std::string& key, const std::string& fallback = {}) const;
    void set(const std::string& key, const std::string& value) { values_[key] = value; }
    [[nodiscard]] const Values& values() const noexcept { return values_; }
    std::filesystem::path path;

private:
    Values values_;
};

}  // namespace mimicry::gui
