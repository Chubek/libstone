#include <TTyUtils/TTyUtils-Button.hpp>
#include <cassert>
using namespace ttyutils::ui;
using ttytk::input::Key;
using ttytk::input::Mouse;

int main() {
    CheckBox check{"Option"};
    check.set_tri_state(true);
    int calls = 0;
    check.on_activate([&] { ++calls; });
    assert(check.handle(Key{U' '}));
    assert(check.state() == CheckBox::State::Checked && calls == 1);
    assert(!check.handle(Key{U' ', 0, 2}));
    check.set_repeat(true);
    assert(check.handle(Key{U' ', 0, 2}));
    assert(check.state() == CheckBox::State::Indeterminate && calls == 2);
    check.set_rect({1, 1, 10, 1});
    assert(check.handle(Mouse{0, 0, 2, 2, true, false}));
    assert(check.handle(Mouse{0, 0, 2, 2, false, false}));
    assert(check.state() == CheckBox::State::Unchecked && calls == 3);

    RadioGroup group;
    Radio one{"one", group};
    one.activate();
    assert(one.selected());
    {
        Radio two{"two", group};
        two.activate();
        assert(!one.selected() && group.selected() == &two);
    }
    assert(group.selected() == nullptr);
    one.activate();
    assert(group.selected() == &one);
}
