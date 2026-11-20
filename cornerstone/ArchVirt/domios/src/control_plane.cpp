#include "control_plane.hpp"

#include "dit.hpp"
#include "shell.hpp"

#include <sstream>

namespace domios {

ControlPlane::ControlPlane(Config config)
    : config_(std::move(config)), plugin_loader_({DOMIOS_PLUGIN_ABI_MAJOR, DOMIOS_PLUGIN_ABI_MINOR, DOMIOS_PLUGIN_ABI_PATCH}) {}

bool ControlPlane::initialize(EventQueue& events, std::string& error) {
    shutdown();
    lua_ = std::make_unique<LiosRuntime>();
    if (!lua_->initialize(config_, events, error)) {
        return false;
    }
    lua_->load_embedded_program("help", "return 'help'");
    plugins_ = plugin_loader_.load(config_, events);
    phase_ = Phase::lua;
    return true;
}

bool ControlPlane::publish_dit(DitPage& page, std::string&) const {
    auto image = serialize_dit(page);
    std::memcpy(&page, image.bytes.data(), image.bytes.size());
    return true;
}

std::string ControlPlane::shell_help() const {
    return "Built-ins resolve before Lua programs: help, exit, info.";
}

std::string ControlPlane::shell_execute(const std::string& line, EventQueue& events) {
    if (!lua_) return "DomIOS is not initialized";
    return execute_shell_line(*lua_, line, events);
}

void ControlPlane::drain_events(EventQueue& events) {
    Event event;
    while (events.try_pop(event)) {
        (void) event;
    }
}

void ControlPlane::shutdown() {
    if (lua_) {
        lua_->shutdown();
    }
    plugin_loader_.shutdown(plugins_);
}

} // namespace domios
