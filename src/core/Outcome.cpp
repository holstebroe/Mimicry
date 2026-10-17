#include "core/Outcome.hpp"

namespace mimicry {

std::string_view to_string(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::Ok: return "Ok";
        case Outcome::Silent: return "Silent";
        case Outcome::NonFinite: return "NonFinite";
        case Outcome::Overload: return "Overload";
        case Outcome::DcRunaway: return "DcRunaway";
        case Outcome::HostError: return "HostError";
        case Outcome::Timeout: return "Timeout";
        case Outcome::Crash: return "Crash";
    }
    return "Unknown";
}

int severity(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::Ok: return 0;
        case Outcome::Silent: return 1;
        case Outcome::NonFinite:
        case Outcome::Overload:
        case Outcome::DcRunaway: return 2;
        case Outcome::HostError:
        case Outcome::Timeout: return 3;
        case Outcome::Crash: return 4;
    }
    return 4;
}

}  // namespace mimicry
