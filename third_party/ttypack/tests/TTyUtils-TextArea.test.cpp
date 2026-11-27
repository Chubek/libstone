#include <TTyUtils/TTyUtils-TextArea.hpp>
#include <cassert>

int main() {
    using namespace ttyutils;
    ui::TextArea area;
    area.set_text("A\u0301猫!\nsecond\nthird");
    assert(area.text() == "A\u0301猫!\nsecond\nthird");
    area.set_rect({0, 0, 12, 2});
    area.jump({2, 0});
    ui::Canvas canvas{{0, 0, 12, 2}};
    area.render(canvas);
    assert(area.first_row() == 1);
    assert(canvas.texts().size() == 4 && canvas.texts().back().value == "third");

    area.jump({0, 1});
    assert(area.handle(ttytk::input::Key{127}));
    assert(area.text().starts_with("猫!"));
    assert(area.handle(ttytk::input::Key{U'z', 4}));
    assert(area.text().starts_with("A\u0301猫!") && area.cursor().cursors().front().position().col == 1);

    area.cursor().add({1, 1});
    assert(area.handle(ttytk::input::Paste{"X"}));
    assert(area.text() == "A\u0301X猫!\nsXecond\nthird");
    assert(area.matches(search::Query::literal("X")).size() == 2);

    area.set_text("猫猫猫");
    area.options().wrap = true;
    area.gutter().show_line_numbers(false);
    area.set_rect({0, 0, 4, 2});
    ui::Canvas wrapped{{0, 0, 4, 2}};
    area.render(wrapped);
    assert(wrapped.texts().size() == 2 && wrapped.texts().front().value == "猫猫");
}
