#include "mahak/Plugin.hpp"

#include <cstdio>
#include <cstdlib>

#ifndef MISSING_ENTRY
using namespace moonstone::mahak;

namespace {
int marker;

OracleResult judge(void *data, const CasePlan &, const CommandResult &command) {
#ifdef BAD_VERDICT
    (void)data;
    (void)command;
    return {true, Validity::InvalidOutput, "inconsistent fixture verdict"};
#else
    const bool passed = data == &marker && command.stdout_text == "plugin stdout\n";
    return {passed, passed ? Validity::Valid : Validity::InvalidOutput, "fixture judged stdout"};
#endif
}

void teardown(void *) noexcept {
    if (const char *path = std::getenv("MAHAK_PLUGIN_TEARDOWN_LOG")) {
        if (auto *file = std::fopen(path, "a")) {
            std::fputs("teardown\n", file);
            std::fclose(file);
        }
    }
}

PluginDescriptor descriptors[] = {
    {sizeof(PluginDescriptor), mahak_plugin_abi_version, make_string_view("fixture.oracle"),
     make_string_view("1"), make_string_view("test oracle"), PluginKind::Oracle,
     PluginCapability::Judge, &marker},
    {sizeof(PluginDescriptor), mahak_plugin_abi_version, make_string_view("fixture.second"),
     {}, {}, PluginKind::Oracle, PluginCapability::Judge, &marker},
};
PluginHooks hooks[] = {{sizeof(PluginHooks), judge}, {sizeof(PluginHooks), judge}};
} // namespace

extern "C" int mahak_plugin_init_v1(const PluginHostApi &host,
                                    PluginRegistration &registration) noexcept {
    PluginHostApi sdk;
    mahak_sdk_fill_host_api(&sdk);
    if (mahak_sdk_version() != host.abi_version || sdk.abi_version != host.abi_version ||
        !sdk.allocate || !sdk.deallocate || !sdk.log ||
        !host.allocate || !host.deallocate || !host.log)
        return 1;
    void *memory = host.allocate(host.host_data, 16);
    if (!memory)
        return 2;
    host.deallocate(host.host_data, memory);
    registration.descriptors = descriptors;
    registration.hooks = hooks;
    registration.count = 1;
    registration.teardown = teardown;
#ifdef BAD_ABI
    registration.count = 2;
    descriptors[1].abi_version = 999;
#endif
#ifdef BAD_SIZE
    hooks[0].struct_size = 0;
#endif
#ifdef MISSING_HOOK
    hooks[0].judge = nullptr;
#endif
    return 0;
}
#else
extern "C" int other_entry() { return 0; }
#endif
