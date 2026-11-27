#include <TTyUtils/TTyUtils-TabBar.hpp>
#include <cassert>
using namespace ttyutils::ui;
using ttytk::input::Key;
using ttytk::input::Mouse;

int main() {
    TabBar bar;
    bar.set_rect({0, 0, 24, 3});
    bar.add({"main", true});
    bar.add({"second"});
    bar.add({"next"});
    assert(bar.handle(Key{U'2', 4}));
    assert(bar.tabs()[bar.selected()].title == "second");
    bar.reorder(0, 2);
    assert(bar.selected() == 0 && bar.tabs()[0].title == "second");
    bar.select(2);
    bar.close(0);
    assert(bar.selected() == 1 && bar.tabs()[1].title == "main");
    bar.set_rect({0, 0, 6, 3});
    bar.set_policy(TabPolicy::PickList);
    Canvas canvas(bar.rect());
    bar.render(canvas);
    assert(canvas.texts().back().value == "…");
    assert(bar.handle(Mouse{0, 0, 6, 1, true, false}));
    assert(bar.handle(Key{U'\r'}));
    assert(bar.selected() == 0);
    bar.close(0);
    bar.close(0);
    assert(bar.tabs().empty() && bar.selected() == 0);
}
