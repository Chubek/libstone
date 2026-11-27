#include <TTyTk/TTyTk-Selection.hpp>
#include <cassert>
#include <string_view>

int main() {
    ttytk::CellBuffer buffer{3, 6};
    for (std::size_t row = 0; row < buffer.rows(); ++row) {
        const auto text = std::array<std::string_view, 3>{"ab cd!", "12 345", "abcdef"}[row];
        for (std::size_t col = 0; col < text.size(); ++col) buffer.put(row, col, {std::string(1, text[col])});
    }
    ttytk::Selection selection;
    using Mode = ttytk::Selection::Mode;
    assert(selection.get_rects().empty());
    selection.start(2, 1);
    selection.update(0, 3);
    assert(selection.to_text(buffer) == "cd!\n12 345\nab");
    selection.start(2, 3, Mode::Block);
    selection.update(0, 1);
    assert(selection.to_text(buffer) == "b c\n2 3\nbcd");
    selection.start(0, 4, Mode::Word);
    assert(selection.to_text(buffer) == "cd");
    selection.update(1, 1);
    assert(selection.to_text(buffer) == "cd!\n12");
    selection.start(1, 2, Mode::Line);
    assert(selection.to_text(buffer) == "12 345");
    selection.start(3, 0);
    assert(selection.to_text(buffer).empty());
    buffer.put(0, 0, {"\xE5\xAD\x97"});
    selection.start(0, 1);
    assert(selection.to_text(buffer) == "\xE5\xAD\x97");
    assert((selection.get_rects(buffer) == std::vector<ttytk::SelectionRect>{{0, 0, 1}}));
}
