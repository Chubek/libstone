#include "config.hpp"

#include "event_queue.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <toml++/toml.hpp>

namespace domios {
namespace {

std::filesystem::path xdg_config_home() {
    const char* env = std::getenv("XDG_CONFIG_HOME");
    if (env && *env) {
        return env;
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::filesystem::path(home) / ".config";
    }
    return {};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

void warn(EventQueue* warnings, const std::string& message, const std::string& file) {
    if (warnings) {
        warnings->try_push({EventKind::log, 0, file, message});
    }
}

} // namespace

std::string default_config_toml() {
    return R"(graphics = "auto"
theme = "default"
locale = "C"
plugin_ignore_list = []
extra_extension_dirs = []
extra_program_dirs = []
extra_plugin_dirs = []
dit_base_override = 0
)";
}

std::filesystem::path first_existing_config_path() {
    const auto xdg = xdg_config_home();
    if (!xdg.empty()) {
        auto xdg_path = xdg / "domweave" / "ios" / "Config.toml";
        if (std::filesystem::exists(xdg_path)) {
            return xdg_path;
        }
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        auto fallback = std::filesystem::path(home) / ".config" / "domweave" / "ios" / "Config.toml";
        if (std::filesystem::exists(fallback)) {
            return fallback;
        }
    }
    return {};
}

Config::GraphicsChoice parse_graphics(const toml::table& table, const std::string& source, std::string& error) {
    if (auto s = table["graphics"].value<std::string>()) {
        if (*s == "auto") return Config::GraphicsChoice::auto_backend;
        if (*s == "vga") return Config::GraphicsChoice::vga;
        if (*s == "virtio-gpu") return Config::GraphicsChoice::virtio_gpu;
    }
    error = source + ": invalid graphics backend";
    return Config::GraphicsChoice::auto_backend;
}

bool validate_config(const Config& config, const std::string& source, std::string& error) {
    if (config.theme.empty()) {
        error = source + ": theme may not be empty";
        return false;
    }
    return true;
}

Config parse_config_text(const std::string& text, const std::string& source, EventQueue* warnings) {
    Config config;
    toml::table table;
    try {
        table = toml::parse(text, source);
    } catch (const toml::parse_error& err) {
        throw std::runtime_error(std::string("failed to parse config: ") + std::string(err.description()));
    }

    std::string error;
    if (table.contains("graphics")) {
        config.graphics = parse_graphics(table, source, error);
        if (!error.empty()) {
            throw std::runtime_error(error);
        }
    }
    if (auto v = table["theme"].value<std::string>()) {
        config.theme = *v;
    }
    if (auto v = table["locale"].value<std::string>()) {
        config.locale = *v;
    }
    if (auto arr = table["plugin_ignore_list"].as_array()) {
        for (const auto& node : *arr) {
            if (auto s = node.value<std::string>()) {
                config.plugin_ignore_list.push_back(*s);
            } else {
                throw std::runtime_error(source + ": plugin_ignore_list must contain strings");
            }
        }
    }
    for (const auto& key : {"extra_extension_dirs", "extra_program_dirs", "extra_plugin_dirs"}) {
        if (auto arr = table[key].as_array()) {
            std::vector<std::filesystem::path>* out = nullptr;
            if (std::string(key) == "extra_extension_dirs") out = &config.extra_extension_dirs;
            if (std::string(key) == "extra_program_dirs") out = &config.extra_program_dirs;
            if (std::string(key) == "extra_plugin_dirs") out = &config.extra_plugin_dirs;
            for (const auto& node : *arr) {
                if (auto s = node.value<std::string>()) {
                    out->emplace_back(*s);
                } else {
                    throw std::runtime_error(source + ": " + key + " must contain strings");
                }
            }
        }
    }
    if (auto i = table["dit_base_override"].value<std::uint64_t>()) {
            if (*i != 0) {
                config.dit_base_override = *i;
            }
    }
    for (const auto& [k, _] : table) {
        const auto key = std::string(k.str());
        if (key != "graphics" && key != "theme" && key != "locale" && key != "plugin_ignore_list" &&
            key != "extra_extension_dirs" && key != "extra_program_dirs" && key != "extra_plugin_dirs" &&
            key != "dit_base_override") {
            warn(warnings, "unknown key " + key, source);
        }
    }
    std::string validation_error;
    if (!validate_config(config, source, validation_error)) {
        throw std::runtime_error(validation_error);
    }
    return config;
}

Config load_config(std::string& source, EventQueue* warnings) {
    if (auto path = first_existing_config_path(); !path.empty()) {
        source = path.string();
        return parse_config_text(read_file(path), source, warnings);
    }
    source = "<embedded-defaults>";
    return parse_config_text(default_config_toml(), source, warnings);
}

} // namespace domios
