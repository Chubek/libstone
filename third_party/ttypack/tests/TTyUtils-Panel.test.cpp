#include <TTyUtils/TTyUtils-Panel.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::ui;
    Panel panel{"界", Border::Rounded};
    panel.set_rect({0, 0, 9, 5});
    panel.set_title_pos(TitlePos::Right);
    auto content = std::make_unique<Widget>();
    auto* child = content.get();
    panel.set_child(std::move(content));
    assert(child->parent() == &panel && panel.children().front().get() == child);

    Canvas canvas(panel.rect());
    panel.render(canvas);
    assert(!canvas.texts().empty() && canvas.texts().front().value == "╭");
    const auto title = canvas.texts().back();
    assert(title.x == 6 && title.y == 0 && title.value == "界");
    assert((child->rect() == Rect{1, 1, 7, 3}));
    assert(panel.hit_test(2, 2).widget == child);

    panel.set_shadow({true, 1, 1});
    Canvas ascii(panel.rect(), false);
    panel.render(ascii);
    assert((child->rect() == Rect{1, 1, 6, 2}));
    assert(std::any_of(ascii.texts().begin(), ascii.texts().end(), [](const auto& text) {
        return text.value == "+" && text.x == 0 && text.y == 0;
    }));
    assert(std::any_of(ascii.texts().begin(), ascii.texts().end(), [](const auto& text) {
        return text.value == "." && text.x == 8 && text.y == 4;
    }));

    panel.set_collapsed(true);
    assert(!child->visible());
    assert(panel.hit_test(2, 2).widget == &panel);
    panel.set_collapsed(false);
    assert(child->visible());
    panel.set_child(nullptr);
    assert(!panel.child());
}
