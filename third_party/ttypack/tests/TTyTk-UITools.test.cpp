#include <TTyTk/TTyTk-UITools.hpp>
#include <cassert>

int main() {
    const ttytk::ui::Box box{ttytk::ui::DoubleBorder};
    for (std::size_t width = 1; width <= 3; ++width) {
        for (std::size_t height = 1; height <= 3; ++height) {
            const auto lines = box.draw(width, height);
            assert(lines.size() == height);
            for (const auto& line : lines) assert(ttytk::layout::measure(line) == static_cast<int>(width));
        }
    }
    ttytk::ui::Layout layout;
    for (const auto text : {"one", "two", "three"}) layout.add(ttytk::ui::Label{text});
    using Slot = ttytk::ui::Layout::Slot;
    assert((layout.grid(5, 3, 2) == std::vector{Slot{0, 0, 3, 2}, Slot{0, 3, 2, 2}, Slot{2, 0, 3, 1}}));
    assert((layout.flex(5, 2, true).back() == Slot{2, 0, 5, 0}));
    assert(layout.labels().front().draw(1) == "…");
    assert(ttytk::ui::Panel{box}.draw(2, 2) == box.draw(2, 2));
}
