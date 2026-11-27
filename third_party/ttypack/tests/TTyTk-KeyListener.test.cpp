#include <TTyTk/TTyTk-KeyListener.hpp>

#include <cassert>

int main() {
    ttytk::KeyListener listener;
    bool callback = false;
    bool observed = false;
    listener.bind("Ctrl-C", [&] { callback = true; });
    listener.on_key = [&](const ttytk::input::Key&) { observed = true; };
    listener.dispatch({U'c', 4, 1}, "Ctrl-C");
    assert(callback && observed);

    unsigned exact{}, sequence{}, up{};
    std::string raw;
    listener.on_raw = [&](std::string_view bytes) { raw += bytes; };
    listener.bind("Ctrl-X", [&] { ++exact; });
    listener.bind("Ctrl-X Ctrl-C", [&] { ++sequence; });
    listener.bind("Up", [&] { ++up; });
    listener.dispatch({U'x', 4, 1}, {}, "\x18");
    assert(raw == "\x18" && exact == 0);
    listener.dispatch({U'c', 4, 3});
    assert(exact == 0 && sequence == 0);
    listener.dispatch({U'c', 4, 1});
    assert(exact == 0 && sequence == 1);
    listener.dispatch({U'x', 4, 1});
    listener.dispatch({57352});
    assert(exact == 1 && up == 1);
    listener.dispatch({U'x', 4, 1});
    listener.reset();
    listener.flush();
    assert(exact == 1);
    listener.dispatch({U'x', 4, 1});
    listener.flush();
    assert(exact == 2);
    listener.bind("Up", [&] { listener.bind("Up", {}); ++up; });
    listener.dispatch({57352});
    listener.dispatch({57352});
    assert(up == 2);
}
