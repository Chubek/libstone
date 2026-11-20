#include "backend.hpp"
#include "config.hpp"
#include "dit.hpp"
#include "event_queue.hpp"
#include "shell.hpp"
#include "lua_runtime.hpp"
#include "bootstrap.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace domios;

int main() {
    {
        auto page = make_default_dit();
        auto image = serialize_dit(page);
        assert(image.bytes.size() == kDitPageSize);
        (void)image;
    }
    {
        auto cfg = parse_config_text(default_config_toml(), "<default>");
        assert(cfg.theme == "default");
    }
    {
        EventQueue q(2);
        q.try_push({EventKind::log, 0, "a", "1"});
        q.try_push({EventKind::log, 0, "b", "2"});
        q.try_push({EventKind::log, 0, "c", "3"});
        Event e;
        assert(q.try_pop(e));
    }
    {
        LiosRuntime lua;
        Config cfg;
        EventQueue q(4);
        std::string err;
        assert(lua.initialize(cfg, q, err));
        assert(lua.sandbox_intact());
    }
    {
        LiosRuntime lua;
        Config cfg;
        EventQueue q(4);
        std::string err;
        lua.initialize(cfg, q, err);
        lua.load_embedded_program("hello", "return 'hello'");
        assert(execute_shell_line(lua, "help", q).find("built-ins") != std::string::npos);
        assert(execute_shell_line(lua, "hello", q) == "hello");
    }
    {
        Bootstrap bootstrap;
        SplashFrame splash;
        std::string err;
        assert(bootstrap.render_splash(splash, err));
        assert(splash.geometry.width == 800);
        assert(splash.geometry.height == 600);
        assert(splash.geometry.pitch == 800 * 4);
        assert(!splash.pixels.empty());
        (void)bootstrap;
    }
    {
        std::filesystem::path tmp = std::filesystem::temp_directory_path() / "domios_config.toml";
        std::ofstream out(tmp);
        out << "graphics = \"vga\"\n";
        out.close();
        auto old = std::getenv("XDG_CONFIG_HOME");
        (void) old;
    }
    return 0;
}
