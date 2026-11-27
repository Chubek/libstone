#include <TTyUtils/TTyUtils-KeyMap.hpp>
#include <cassert>

int main() {
    using namespace std::chrono_literals;
    using namespace ttyutils::keymap;
    Map map;
    assert((parse_key("\t<leader>gd C-s  ").keys == std::vector<std::string>{"<leader>", "g", "d", "C-s"}));
    map.bind("C-x C-s", "save");
    map.bind("C-x C-q", "quit");
    map.bind("C-x", "prefix-action");
    assert(map.lookup(" C-x\t C-s  ") == "save");
    assert(!map.lookup(""));

    const Map::Clock::time_point now{};
    map.set_timeout(10ms);
    std::vector<std::string> hints;
    map.on_pending([&](auto value) { hints = std::move(value); });
    assert(!map.feed("C-x", now));
    assert((hints == std::vector<std::string>{"C-q", "C-s"}));
    assert(!map.expire(now + 5ms));
    assert(map.feed("C-s", now + 6ms) == "save" && hints.empty());
    assert(!map.feed("C-x", now));
    assert(map.expire(now + 10ms) == "prefix-action" && hints.empty());
    assert(!map.feed("C-x", now));
    assert(!map.feed("C-s", now + 10ms));

    map.bind("insert", "C-x C-s", "insert-save");
    map.set_mode("insert");
    assert(map.lookup("C-x C-s") == "insert-save");
    assert(map.lookup("C-x C-q") == "quit");

    ttyutils::config::Store cfg;
    cfg.set("keys.go", "<leader>gd");
    map.set_leader(",");
    map.bind_config(cfg, "keys");
    assert(!map.feed(",", now));
    assert(!map.feed("g", now));
    assert(map.feed("d", now) == "go");
}
