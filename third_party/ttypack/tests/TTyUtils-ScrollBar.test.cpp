#include <TTyUtils/TTyUtils-ScrollBar.hpp>
#include <cassert>
#include <limits>
using namespace ttyutils::ui;

struct Scroll : Scrollable {
    std::size_t at{};
    std::size_t total{10};
    std::size_t offset() const override { return at; }
    void scroll_to(std::size_t v) override { at = v; }
    std::size_t length() const override { return total; }
    std::size_t visible_length() const override { return 4; }
};

int main() {
    Scroll model;
    ScrollBar bar{Horizontal};
    bar.attach(model);
    bar.set_rect({0, 0, 10, 1});
    bar.scroll_by(3);
    assert(model.at == 3 && bar.fraction() == 0.5);
    bar.scroll_at(9);
    assert(model.at == 6 && bar.fraction() == 1);
    bar.scroll_by(std::numeric_limits<int>::min());
    assert(model.at == 0);
    bar.set_marks({{7, {}}});
    bar.set_ascii(true);
    Canvas canvas(bar.rect());
    bar.render(canvas);
    assert(canvas.texts().size() == 10);
    assert(canvas.texts()[3].value == "#" && canvas.texts()[7].value == "*");
    model.total = std::numeric_limits<std::size_t>::max();
    model.at = model.total - 5;
    bar.scroll_by(std::numeric_limits<int>::max());
    assert(model.at == model.total - 4);
}
