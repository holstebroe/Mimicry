// VST3 backend (docs/spec/02_HOSTING.md section 2, WP1.7). Placeholder until the SDK hosting module is wired.

#include "host/Engine.hpp"

namespace mimicry::host {

expected<std::unique_ptr<Engine>, HostError> open_vst3_engine(const std::filesystem::path&, std::string_view,
                                                              std::shared_ptr<ThreadRoles>) {
    return unexpected(HostError{"VST3 hosting is not compiled in"});
}

}  // namespace mimicry::host
