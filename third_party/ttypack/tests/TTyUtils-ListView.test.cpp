#include <TTyUtils/TTyUtils-ListView.hpp>
#include <cassert>
using namespace ttyutils::ui;

struct Model : ListModel {
    std::size_t count{1'000'000};
    mutable std::size_t fetched{};
    std::size_t size() const override { return count; }
    std::string text(std::size_t row) const override {
        ++fetched;
        return row % 2 == 0 ? "even" : "odd";
    }
};

int main() {
    auto model = std::make_shared<Model>();
    ListView view{model};
    view.set_rect({0, 0, 5, 2});
    view.select(999'999);
    Canvas canvas(view.rect());
    view.render(canvas);
    assert(model->fetched == 2 && view.offset() == 999'998);
    assert(canvas.texts().size() == 2);

    model->count = 5;
    view.set_filter("odd");
    Canvas filtered(view.rect());
    view.render(filtered);
    assert(filtered.texts().size() == 2 && filtered.texts()[1].y == 1);
    assert(view.selected() == 1);
    assert(view.handle(ttytk::input::Key{57353}));
    assert(view.selected() == 3);
    view.toggle_selected();
    assert(view.selection().contains(3));
    int chosen = -1;
    view.on_choose([&](int row) { chosen = row; });
    view.choose();
    assert(chosen == 3);
    view.set_filter("none");
    view.choose();
    assert(chosen == 3 && view.length() == 0);
}
