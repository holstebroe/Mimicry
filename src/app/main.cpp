// mimicry: supervisor / command line (docs/spec/01_ARCHITECTURE.md section 6).
// Skeleton: the commands exist so that the interface is visible; they are implemented in order of
// docs/PROJECT_PLAN.md.

#include <cstdio>
#include <format>
#include <string>
#include <string_view>

#include "core/Version.hpp"
#include "host/PluginHost.hpp"

namespace {

struct Command {
    std::string_view name;
    std::string_view usage;
    std::string_view summary;
};

constexpr Command kCommands[] = {
    {"list", "list <plugin>", "enumerate parameters with units and flags"},
    {"probe", "probe <plugin>", "sensitivity, safe range and hazard probe; writes the plugin profile"},
    {"fit", "fit --plugin P --wav T.wav", "match a WAV"},
    {"transfer", "transfer --source A --target B", "match source presets in another plugin (any format mix)"},
    {"image", "image --plugin P --image I.png", "match an image"},
    {"fingerprint", "fingerprint <plugin>", "build the random-preset feature database"},
    {"rehearse", "rehearse <plugin>", "round-trip self-test"},
    {"eval", "eval --plugin P --state S --wav T.wav", "score a state without fitting"},
    {"replay", "replay <crash.json>", "re-run a recorded failure in-process"},
};

void print_usage() {
    std::string text = std::format("mimicry {}: preset matcher for CLAP and VST3 plugins\n\nusage: mimicry <command> ...\n\n",
                                   mimicry::version);
    for (const auto& c : kCommands) text += std::format("  {:<40} {}\n", c.usage, c.summary);
    text += "\n  --version, --help\n\n<plugin> is a .clap or .vst3 path. Most commands are not implemented yet;\n"
            "see docs/PROJECT_PLAN.md for the order.\n";
    std::fputs(text.c_str(), stdout);
}

int not_implemented(std::string_view command) {
    std::fputs(std::format("mimicry {}: not implemented yet (see docs/PROJECT_PLAN.md)\n", command).c_str(), stderr);
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        print_usage();
        return 0;
    }
    if (cmd == "--version") {
        std::fputs(std::format("mimicry {}\n", mimicry::version).c_str(), stdout);
        return 0;
    }

    for (const auto& c : kCommands) {
        if (c.name != cmd) continue;
        if (cmd == "list" && argc >= 3) {
            // Format detection works already; hosting itself arrives with WP1.2 / WP1.7.
            const auto format = mimicry::host::format_from_path(argv[2]);
            if (!format) {
                std::fputs(std::format("mimicry list: {}\n", format.error()).c_str(), stderr);
                return 1;
            }
            std::fputs(std::format("{}: {} plugin\n", argv[2], mimicry::host::to_string(*format)).c_str(), stdout);
        }
        return not_implemented(cmd);
    }
    std::fputs(std::format("mimicry: unknown command '{}' (try --help)\n", cmd).c_str(), stderr);
    return 1;
}
