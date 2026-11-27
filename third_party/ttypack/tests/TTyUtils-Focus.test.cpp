#include <TTyUtils/TTyUtils-Focus.hpp>
#include <cassert>

int main() {
    ttyutils::ui::Widget root;
    root.set_rect({0, 0, 10, 2});
    bool focused = false;
    root.on_focus([&](bool value) { focused = value; });
    ttyutils::ui::FocusChain chain{root};
    assert(chain.next() == &root);
    assert(focused);
    using namespace ttyutils::ui;
    auto a = std::make_unique<Widget>();
    auto b = std::make_unique<Widget>();
    auto* left = a.get();
    auto* right = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    left->set_rect({0, 0, 3, 2});
    right->set_rect({6, 0, 3, 2});
    right->set_tab_order(-1);
    root.set_focusable(false);
    assert(focus_next(root) == right && !focused);
    assert(focus_next(root) == left && !right->focused());
    assert(focus_prev(root) == right);
    assert(chain.move(FocusDirection::Left) == left);
    {
        FocusScope scope(*right);
        assert(FocusScope::active() == right);
        assert(focus_next(root) == right && right->focused());
        assert(!left->focused());
        Widget independent;
        assert(focus_prev(independent) == &independent);
    }
    assert(!FocusScope::active());
    assert(chain.focus(*left) == left);
    assert(chain.move(FocusDirection::Right) == right);
    right->set_visible(false);
    assert(focus_prev(root) == left && !right->focused());
    return 0;
}
