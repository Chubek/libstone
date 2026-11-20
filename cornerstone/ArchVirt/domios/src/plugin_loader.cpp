#include "plugin_loader.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sstream>

#include <dynalo/dynalo.hpp>

namespace domios {
namespace {

using Library = dynalo::library;

std::string native_plugin_name(const std::filesystem::path& path) {
    return path.filename().string();
}

bool ignored(const Config& config, const std::string& name) {
    return std::find(config.plugin_ignore_list.begin(), config.plugin_ignore_list.end(), name) != config.plugin_ignore_list.end();
}

} // namespace

PluginLoader::PluginLoader(const ImageManifestVersion& manifest_abi) : manifest_abi_(manifest_abi) {}

std::vector<LoadedPlugin> PluginLoader::load(const Config& config, EventQueue& events) {
    std::vector<LoadedPlugin> plugins;
    std::vector<std::filesystem::path> dirs;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        dirs.emplace_back(std::filesystem::path(xdg) / "domweave" / "ios" / "plugins");
    }
    for (const auto& dir : config.extra_plugin_dirs) {
        dirs.push_back(dir);
    }
    for (const auto& dir : dirs) {
        if (!std::filesystem::exists(dir)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto name = native_plugin_name(entry.path());
            if (ignored(config, name)) {
                events.try_push({EventKind::plugin_notice, 0, name, "ignored by config"});
                continue;
            }
            try {
                auto lib = std::make_unique<Library>(entry.path().string());
                auto entry_fn = lib->get_function<const DomIOSPluginDescriptor*()>("domios_plugin_entry");
                if (!entry_fn) {
                    events.try_push({EventKind::plugin_notice, 0, name, "missing entry symbol"});
                    continue;
                }
                const auto* descriptor = entry_fn();
                if (!descriptor) {
                    events.try_push({EventKind::plugin_notice, 0, name, "entry returned null descriptor"});
                    continue;
                }
                if (descriptor->abi.major != manifest_abi_.major || descriptor->abi.minor != manifest_abi_.minor || descriptor->abi.patch != manifest_abi_.patch) {
                    events.try_push({EventKind::plugin_notice, 0, name, "ABI mismatch"});
                    continue;
                }
                void* state = nullptr;
                if (!descriptor->initialize || descriptor->initialize(nullptr, &state) != 0) {
                    events.try_push({EventKind::plugin_notice, 0, name, "initialization failed"});
                    continue;
                }
                auto handle = std::unique_ptr<void, void(*)(void*)>(new dynalo::library(std::move(*lib)),
                    [](void* p) { delete static_cast<dynalo::library*>(p); });
                LoadedPlugin plugin{entry.path(), descriptor->name ? descriptor->name : name,
                                    descriptor->abi, std::move(handle), descriptor, state};
                plugins.push_back(std::move(plugin));
                events.try_push({EventKind::plugin_notice, 0, name, "loaded"});
            } catch (const std::exception& e) {
                events.try_push({EventKind::plugin_notice, 1, name, e.what()});
            }
        }
    }
    return plugins;
}

void PluginLoader::shutdown(std::vector<LoadedPlugin>& plugins) noexcept {
    for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
        if (it->descriptor && it->descriptor->shutdown) {
            it->descriptor->shutdown(it->state);
        }
    }
    plugins.clear();
}

} // namespace domios
