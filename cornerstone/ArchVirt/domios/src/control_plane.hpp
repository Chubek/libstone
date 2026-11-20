#pragma once

#include "config.hpp"
#include "event_queue.hpp"
#include "dit.hpp"
#include "lua_runtime.hpp"
#include "plugin_loader.hpp"

#include <memory>

namespace domios {

class ControlPlane {
public:
    explicit ControlPlane(Config config);
    ~ControlPlane() { shutdown(); }
    bool initialize(EventQueue& events, std::string& error);
    bool publish_dit(DitPage& page, std::string& error) const;
    std::string shell_help() const;
    std::string shell_execute(const std::string& line, EventQueue& events);
    void drain_events(EventQueue& events);
    void shutdown();
    const Config& config() const noexcept { return config_; }
    Phase phase() const noexcept { return phase_; }
    void set_phase(Phase phase) noexcept { phase_ = phase; }
    const std::vector<LoadedPlugin>& plugins() const noexcept { return plugins_; }

private:
    Config config_;
    Phase phase_{Phase::bootstrap};
    std::unique_ptr<LiosRuntime> lua_;
    PluginLoader plugin_loader_;
    std::vector<LoadedPlugin> plugins_;
    std::map<std::string, std::string> builtins_;
};

} // namespace domios
