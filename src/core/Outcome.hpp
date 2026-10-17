#pragma once
// Render outcomes (docs/spec/05_RESILIENCE.md section 2). A failed render is information for the
// search, never silently dropped.

#include <string_view>

namespace mimicry {

enum class Outcome {
    Ok,         // valid audio: all output checks passed
    Silent,     // the parameter combination muted the plugin
    NonFinite,  // NaN or Inf in the output
    Overload,   // peak above the limit (default 50, as Acidus's calibrator)
    DcRunaway,  // unstable filter/feedback: runaway DC
    HostError,  // process/setup returned an error, or the plugin threw across a call
    Timeout,    // hang or pathological CPU use (watchdog)
    Crash,      // the worker process died
};

[[nodiscard]] std::string_view to_string(Outcome outcome) noexcept;

[[nodiscard]] constexpr bool is_valid(Outcome outcome) noexcept { return outcome == Outcome::Ok; }

// Order used to rank invalid candidates behind all valid ones (spec 05 section 3):
// Ok 0 < Silent 1 < Overload = DcRunaway = NonFinite 2 < Timeout = HostError 3 < Crash 4.
[[nodiscard]] int severity(Outcome outcome) noexcept;

}  // namespace mimicry
