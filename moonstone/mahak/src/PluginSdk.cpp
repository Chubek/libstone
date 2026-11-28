#include "mahak/Plugin.hpp"

#include <cstdio>
#include <cstdlib>

extern "C" std::uint32_t mahak_sdk_version() noexcept {
    return moonstone::mahak::mahak_plugin_abi_version;
}

extern "C" void mahak_sdk_fill_host_api(moonstone::mahak::PluginHostApi *host) noexcept {
    if (!host)
        return;
    *host = {};
    host->allocate = [](void *, std::size_t size) noexcept { return std::malloc(size); };
    host->deallocate = [](void *, void *pointer) noexcept { std::free(pointer); };
    host->log = [](void *, int severity, moonstone::mahak::StringView message) noexcept {
        const auto text = message.value();
        std::fprintf(stderr, "mahak plugin [%d]: ", severity);
        if (!text.empty())
            std::fwrite(text.data(), 1, text.size(), stderr);
        std::fputc('\n', stderr);
    };
}
