// mimicry_worker: the only process that loads plugins (docs/spec/05_RESILIENCE.md section 1).
// Skeleton. The supervisor protocol, shared-memory audio and the render core arrive with WP1.4-1.5.

#include <cstdio>
#include <format>
#include <string_view>

#include "core/Version.hpp"

int main(int argc, char** argv) {
    if (argc >= 2 && std::string_view(argv[1]) == "--version") {
        std::fputs(std::format("mimicry_worker {}\n", mimicry::version).c_str(), stdout);
        return 0;
    }
    std::fputs("mimicry_worker: not implemented yet (see docs/PROJECT_PLAN.md, WP1)\n", stderr);
    return 2;
}
