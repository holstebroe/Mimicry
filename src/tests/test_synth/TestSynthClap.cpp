// mimicry_test_synth (STUB): the plugin the host is developed against (docs/spec/07_OUTPUT_VALIDATION.md
// section 5). For now it is a valid CLAP module that exports clap_entry and a plugin factory with no plugins,
// so the module builds and loads. WP1.6 turns it into a small synth with planted hazards, and adds the
// VST3 build through clap-wrapper.

#include <clap/clap.h>

#include <cstring>

namespace {

uint32_t factory_count(const clap_plugin_factory_t*) { return 0; }

const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, uint32_t) { return nullptr; }

const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t*, const char*) {
    return nullptr;
}

const clap_plugin_factory_t kFactory = {factory_count, factory_descriptor, factory_create};

bool entry_init(const char*) { return true; }

void entry_deinit() {}

const void* entry_get_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entry_init,
    entry_deinit,
    entry_get_factory,
};
