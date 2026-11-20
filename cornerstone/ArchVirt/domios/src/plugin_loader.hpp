#pragma once

#include "config.hpp"
#include "event_queue.hpp"
#include "state.hpp"
#include "DomIOSPlugin.h"

#include <memory>
#include <vector>

namespace domios {

struct LoadedPlugin {
    std::filesystem::path path;
    std::string name;
    ::DomIOSVersion abi{};
    std::unique_ptr<void, void(*)(void*)> handle{nullptr, nullptr};
    const ::DomIOSPluginDescriptor* descriptor{};
    void* state{};
};

class PluginLoader {
public:
    explicit PluginLoader(const ImageManifestVersion& manifest_abi);
    std::vector<LoadedPlugin> load(const Config& config, EventQueue& events);
    void shutdown(std::vector<LoadedPlugin>& plugins) noexcept;

private:
    ImageManifestVersion manifest_abi_;
};

} // namespace domios
