#pragma once

#include "event_queue.hpp"
#include "lua_runtime.hpp"

#include <map>
#include <string>

namespace domios {

std::string execute_shell_line(LiosRuntime& lua, const std::string& line, EventQueue& events);

} // namespace domios

