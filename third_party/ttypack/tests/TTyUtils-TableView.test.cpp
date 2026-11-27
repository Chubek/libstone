#include <TTyUtils/TTyUtils-TableView.hpp>
#include <cassert>
#include <array>
using namespace ttyutils::ui;

struct Model : TableModel {
    std::array<std::array<std::string, 2>, 4> data{{{"A", "雪"}, {"B", "b"}, {"A", "long"}, {"A", "b"}}};
    std::size_t rows() const override { return data.size(); }
    std::string value(std::size_t row, std::size_t column) const override { return data.at(row).at(column); }
};

int main() {
    TableView view{std::make_shared<Model>()};
    view.add_column({.title = "key", .width = 4});
    view.add_column({.title = "item", .width = 3, .align = Right});
    view.sort_by("key");
    view.sort_by("item", SortOrder::Asc, true);
    view.set_rect({0, 0, 7, 3});
    view.move_column(1, 0);
    Canvas first(view.rect());
    view.render(first);
    assert(first.texts().size() == 6 && first.texts()[0].value == "it…");
    assert(first.texts()[2].value == "  b" && first.texts()[3].value == "A   ");
    assert(first.texts()[4].value == "lo…");
    view.scroll_to(2);
    Canvas last(view.rect());
    view.render(last);
    assert(last.texts()[0].value == "it…" && last.texts()[2].value == " 雪");
    assert(last.texts()[5].value == "B   ");
}
