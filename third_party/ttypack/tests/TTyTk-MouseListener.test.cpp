#include <TTyTk/TTyTk-MouseListener.hpp>

#include <cassert>

int main() {
    ttytk::MouseListener listener;
    unsigned clicks = 0;
    listener.hit_test = [](const ttytk::input::Mouse& mouse) { return mouse.x == 4; };
    listener.on_click = [&](const ttytk::input::Mouse&) { ++clicks; };
    listener.dispatch({0, 0, 4, 2, true, false});
    listener.dispatch({0, 0, 5, 2, true, false});
    assert(clicks == 1);

    using namespace std::chrono_literals;
    listener.set_region(ttytk::MouseListener::Region{4, 2, 3, 3});
    listener.on_click = [&](const ttytk::input::Mouse& mouse) {
        assert(mouse.x == 1 && mouse.y == 2);
        ++clicks;
    };
    const ttytk::MouseListener::Clock::time_point initial{};
    listener.dispatch({0, 0, 4, 3, true, false}, initial);
    listener.dispatch({0, 0, 4, 3, true, false}, initial + 50ms);
    listener.dispatch({0, 0, 4, 5, true, false}, initial + 70ms);
    assert(clicks == 3 && listener.click_count() == 2);
    unsigned moves{}, drags{}, releases{};
    listener.on_move = [&](const auto&) { ++moves; };
    listener.on_drag = [&](const auto&) { ++drags; };
    listener.on_release = [&](const auto&) { ++releases; };
    listener.dispatch({3, 0, 4, 3, false, true});
    listener.dispatch({0, 0, 4, 3, true, true});
    listener.dispatch({0, 0, 4, 3, false, false});
    assert(moves == 1 && drags == 1 && releases == 1 && listener.click_count() == 0);
}
