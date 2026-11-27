#include <TTyUtils/TTyUtils-Layout.hpp>
#include <cassert>
#include <memory>

struct Hinted : ttyutils::ui::Widget {
    Hinted(int minimum, int preferred, int stretch) : size{{minimum, 1}, {preferred, 1}, stretch} {}
    [[nodiscard]] auto hint() const -> ttyutils::ui::SizeHint override { return size; }
    ttyutils::ui::SizeHint size;
};

int main() {
    using namespace ttyutils::ui;
    HBox box;
    box.add_child(std::make_unique<Spacer>());
    box.add_child(std::make_unique<Spacer>());
    box.set_rect({0, 0, 5, 1});
    box.layout();
    assert(box.children()[0]->rect().width == 3);
    assert(box.children()[1]->rect().width == 2);

    HBox flex;
    flex.add_child(std::make_unique<Hinted>(2, 4, 1));
    flex.add_child(std::make_unique<Hinted>(3, 4, 3));
    flex.set_rect({0, 0, 16, 1});
    Canvas canvas(flex.rect());
    flex.render(canvas);
    assert(flex.children()[0]->rect().width == 6 && flex.children()[1]->rect().width == 10);
    flex.set_rect({0, 0, 5, 1});
    flex.layout();
    assert(flex.children()[0]->rect().width == 2 && flex.children()[1]->rect().width == 3);

    Grid grid(2, 3);
    grid.add_child(std::make_unique<Widget>());
    grid.place(0, {0, 1, 2, 2});
    grid.set_rect({4, 8, 8, 5});
    grid.layout();
    assert((grid.children()[0]->rect() == Rect{7, 8, 5, 5}));

    Split split;
    split.add_child(std::make_unique<Hinted>(4, 4, 1));
    split.add_child(std::make_unique<Hinted>(3, 3, 1));
    split.set_rect({0, 0, 19, 1});
    split.drag_to(1);
    split.layout();
    assert(split.children()[0]->rect().width == 4);
    split.drag_to(18);
    split.layout();
    assert(split.children()[0]->rect().width == 16);
    return 0;
}
