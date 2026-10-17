// Used when VST3 hosting is switched off (MIMICRY_VST3_HOSTING=OFF).

#include "host/Engine.hpp"

namespace mimicry::host {

expected<std::unique_ptr<Engine>, HostError> open_vst3_engine(const std::filesystem::path&, std::string_view,
                                                              std::shared_ptr<ThreadRoles>) {
    return unexpected(HostError{"VST3 hosting is not compiled in (MIMICRY_VST3_HOSTING=OFF)"});
}

}  // namespace mimicry::host
