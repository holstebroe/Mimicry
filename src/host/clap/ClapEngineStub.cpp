// Used when the clap submodule is missing, so that the rest of the tree still builds.

#include "host/Engine.hpp"

namespace mimicry::host {

expected<std::unique_ptr<Engine>, HostError> open_clap_engine(const std::filesystem::path&, std::string_view,
                                                              std::shared_ptr<ThreadRoles>) {
    return unexpected(HostError{"CLAP hosting is not compiled in (clap submodule missing)"});
}

}  // namespace mimicry::host
