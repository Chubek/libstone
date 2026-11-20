#pragma once

#include "event_queue.hpp"
#include "state.hpp"

#include <string>

namespace domios {

std::string default_config_toml();
Config load_config(std::string& source, EventQueue* warnings = nullptr);
Config parse_config_text(const std::string& text, const std::string& source, EventQueue* warnings = nullptr);
std::filesystem::path first_existing_config_path();
bool validate_config(const Config& config, const std::string& source, std::string& error);

} // namespace domios
