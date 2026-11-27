#include <TTyUtils/TTyUtils-StatusBar.hpp>
#include <cassert>
using namespace ttyutils::ui;
using namespace std::chrono_literals;

int main() {
    StatusBar bar;
    int hits = 0;
    ttyutils::theme::Style style;
    style.attributes = 1;
    bar.left({{"雪", style, 2, [&] { ++hits; }}, transient("expired", 0ms)});
    bar.center({{"hidden", {}, -1}});
    bar.right({{"99", {}, 3}});
    bar.set_rect({1, 2, 5, 1});
    Canvas canvas(bar.rect());
    bar.render(canvas);
    assert(canvas.texts().size() == 2);
    assert(canvas.texts()[0].style == style && canvas.texts()[0].value == "雪");
    assert(canvas.texts()[1].x == 4);
    assert(bar.click(2, 2) && hits == 1);
    bar.flash("ok", 10s);
    Canvas flashed(bar.rect());
    bar.render(flashed);
    assert(flashed.texts().size() == 1 && flashed.texts()[0].value == "ok");
    assert(!bar.click(2, 2));
    bar.flash("already expired", 0ms);
    assert(bar.message().empty());
}
