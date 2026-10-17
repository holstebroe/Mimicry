// Smoke test for the skeleton: outcomes, format detection, the interface stub. Returns non-zero on
// failure, like tannhauser_dsp_test.

#include <cstdio>
#include <string_view>

#include "core/Outcome.hpp"
#include "core/Version.hpp"
#include "host/PluginHost.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    using mimicry::Outcome;
    using mimicry::host::PluginFormat;

    check(!mimicry::version.empty(), "version is set");

    // Outcomes: names and the ranking of invalid candidates (spec 05 section 3).
    check(mimicry::to_string(Outcome::Ok) == "Ok", "Outcome::Ok name");
    check(mimicry::to_string(Outcome::Crash) == "Crash", "Outcome::Crash name");
    check(mimicry::is_valid(Outcome::Ok) && !mimicry::is_valid(Outcome::Silent), "only Ok is valid");
    check(mimicry::severity(Outcome::Ok) < mimicry::severity(Outcome::Silent), "Ok < Silent");
    check(mimicry::severity(Outcome::Silent) < mimicry::severity(Outcome::NonFinite), "Silent < NonFinite");
    check(mimicry::severity(Outcome::NonFinite) == mimicry::severity(Outcome::Overload) &&
              mimicry::severity(Outcome::Overload) == mimicry::severity(Outcome::DcRunaway),
          "NonFinite = Overload = DcRunaway");
    check(mimicry::severity(Outcome::DcRunaway) < mimicry::severity(Outcome::Timeout), "DcRunaway < Timeout");
    check(mimicry::severity(Outcome::Timeout) == mimicry::severity(Outcome::HostError), "Timeout = HostError");
    check(mimicry::severity(Outcome::HostError) < mimicry::severity(Outcome::Crash), "HostError < Crash");

    // Format from the path.
    using mimicry::host::format_from_path;
    check(format_from_path("a/b/synth.clap") == PluginFormat::Clap, ".clap");
    check(format_from_path("synth.CLAP") == PluginFormat::Clap, ".CLAP (case-insensitive)");
    check(format_from_path("C:/Program Files/Common Files/VST3/synth.vst3") == PluginFormat::Vst3, ".vst3");
    check(format_from_path("/usr/lib/vst3/synth.vst3/") == PluginFormat::Vst3, ".vst3 bundle with trailing slash");
    check(!format_from_path("synth.dll").has_value(), ".dll rejected");
    check(!format_from_path("synth").has_value(), "no extension rejected");

    // The stub host reports a clear error rather than pretending to host.
    const auto missing = mimicry::host::open("synth.clap");
    check(!missing.has_value() && !missing.error().message.empty(), "open() is an error until the backends exist");
    const auto bad = mimicry::host::open("synth.txt");
    check(!bad.has_value() && !bad.error().message.empty(), "open() rejects unknown extensions");

    if (failures == 0) std::puts("mimicry_smoke_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
