#include <TTyUtils/TTyUtils-Widget.hpp>
#include <cassert>

struct TestWidget final : ttyutils::ui::Widget {
    void paint(ttyutils::ui::Canvas& canvas) override { canvas.text(-1, 1, "ab界é!"); }
};

int main() {
    using namespace ttyutils::ui;
    Widget root;
    root.set_rect({0, 0, 4, 4});
    auto child = std::make_unique<TestWidget>();
    auto* widget = child.get();
    root.add_child(std::move(child));
    widget->set_rect({1, 0, 4, 4});
    Canvas canvas(root.rect());
    root.render(canvas);
    assert(canvas.texts().size() == 1 && canvas.texts()[0].x == 1 && canvas.texts()[0].value == "界é");
    assert(root.hit_test(2, 2).widget == widget);
    assert(!root.hit_test(4, 2).widget);
    assert(root.damage().empty());
    widget->invalidate({3, 1, 2, 1});
    assert((root.damage() == DamageList{{3, 1, 1, 1}}));
    widget->set_visible(false);
    assert(root.hit_test(2, 2).widget == &root);
    assert(widget->parent() == &root);
    auto removed = root.remove_child(widget);
    assert(removed.get() == widget && !removed->parent());
    assert(cell_width("界é") == 3);
    assert(fit_text("界é", 1).empty());
    assert(fit_text("界é", 2) == "界");
    Canvas untrusted({0, 0, 10, 1});
    untrusted.text(0, 0, "\x1b" "a\xc2\x9b" "b\x9b");
    assert(untrusted.texts()[0].value == "ab�");
    return 0;
}
