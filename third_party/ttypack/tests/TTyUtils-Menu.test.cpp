#include <TTyUtils/TTyUtils-Menu.hpp>
#include <cassert>
using namespace ttyutils::ui;
using ttytk::input::Key;

int main() {
    int hits = 0;
    Menu menu{"&File", {{.label = "Divider", .action = [&] { ++hits; }, .separator = true},
                        {.label = "&Open", .accelerator = "C-o", .action = [&] { ++hits; }, .checkable = true}}};
    menu.activate(0);
    assert(hits == 0);
    menu.set_rect({0, 0, 18, 3});
    menu.open();
    Canvas canvas(menu.rect());
    menu.render(canvas);
    assert(canvas.texts().back().value == "C-o" && canvas.texts().back().x == 15);
    assert(menu.handle(Key{U'o', 2}));
    assert(hits == 1 && menu.items()[1].checked && !menu.opened());
    MenuBar bar{std::move(menu), Menu{"&Edit"}};
    bar.set_rect({0, 0, 20, 4});
    assert(bar.handle(Key{U'f', 2}));
    assert(bar.handle(Key{U'\r'}));
    assert(hits == 2 && !bar.menus()[0].items()[1].checked);
}
