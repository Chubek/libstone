#include <TTyTk/TTyTk-Renderer.hpp>
#include <cassert>
#include <sstream>

int main() {
    ttytk::CellBuffer buffer{2, 5};
    ttytk::Renderer renderer;
    ttytk::Damage damage;
    std::ostringstream output;
    const auto reset = [&] { output.str({}); };
    renderer.render(buffer, output, damage);
    assert(output.str().find("\x1b[2;1H") != std::string::npos);
    reset();
    renderer.render(buffer, output);
    assert(output.str().empty());

    buffer.put(0, 1, {"X", 1, {1, 2, 3}, {4, 5, 6}});
    buffer.put(1, 3, {"Y"});
    damage.mark(0, 1, 1);
    renderer.render(buffer, output, damage);
    assert(output.str() == "\x1b[1;2H\x1b[0;1;38;2;1;2;3;48;2;4;5;6mX\x1b[0m");
    reset();
    renderer.render(buffer, output);
    assert(output.str().find("\x1b[2;4H") == 0 && output.str().find('Y') != std::string::npos);

    reset();
    buffer.put(0, 0, {"\xE5\xAD\x97"});
    renderer.render(buffer, output, damage); // damage hits continuation only
    assert(output.str().find("\x1b[1;1H") == 0 && output.str().find("\xE5\xAD\x97") != std::string::npos);
    buffer.put(0, 1, {"Z"});
    reset();
    renderer.render(buffer, output, damage);
    assert(output.str().find("\x1b[1;1H") == 0 && output.str().find(" Z") != std::string::npos);

    reset();
    renderer.set_terminal_state(false);
    renderer.render(buffer, output);
    assert(output.str() == "\x1b[?7l");
    reset();
    renderer.render(buffer, output);
    assert(output.str().empty());
    buffer.resize(1, 2);
    renderer.render(buffer, output, damage);
    assert(output.str().find("\x1b[2J") != std::string::npos);

    reset();
    renderer.reset_cache();
    renderer.render(buffer, output);
    assert(output.str().find("\x1b[?7l") == 0 && output.str().find(" Z") != std::string::npos);

    std::ostringstream different;
    renderer.render(buffer, different, damage);
    assert(different.str().find(" Z") != std::string::npos);
    different.setstate(std::ios::badbit);
    renderer.flush(different);
    different.clear();
    different.str({});
    renderer.render(buffer, different, damage);
    assert(different.str().find(" Z") != std::string::npos);

    ttytk::TermMode modes;
    modes.set(ttytk::Mode::AltScreen);
    modes.set(ttytk::Mode::MouseSgrPixels);
    reset();
    renderer.set_terminal_state(modes);
    renderer.render(buffer, output);
    assert(output.str().find("\x1b[?1049h") != std::string::npos);
    assert(output.str().find("\x1b[?1016h") != std::string::npos);
    assert(output.str().find(" Z") != std::string::npos);
    reset();
    modes.set(ttytk::Mode::MouseSgr);
    renderer.set_terminal_state(modes);
    renderer.render(buffer, output);
    assert(output.str() == "\x1b[?1016l\x1b[?1006h");
}
