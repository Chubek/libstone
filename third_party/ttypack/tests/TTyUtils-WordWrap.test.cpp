#include <TTyUtils/TTyUtils-WordWrap.hpp>
#include <cassert>

int main() {
    ttyutils::textbuf::Buffer buffer{"a bb ccc"};
    auto map = ttyutils::wrap::soft_wrap(buffer.snapshot(), 4);
    assert(map.rows().size() >= 2);
    assert(map.visual_to_buffer({0, 0}).line == 0);
    using Position = ttyutils::textbuf::Buffer::Position;
    ttyutils::textbuf::Buffer wide{"a界🙂b\r\n"};
    const auto cells = ttyutils::wrap::soft_wrap(wide.snapshot(), 3);
    assert(cells.rows().size() == 3);
    assert((cells.rows()[0].end == Position{0, 2}));
    assert((cells.visual_to_buffer({0, 2}) == Position{0, 1}));
    assert(cells.buffer_to_visual({0, 3}).col == 2);
    ttyutils::textbuf::Buffer spaces{"one   two"};
    const auto all = ttyutils::wrap::soft_wrap(spaces.snapshot(), 4);
    for (std::size_t i = 1; i < all.rows().size(); ++i)
        assert(all.rows()[i - 1].end == all.rows()[i].start);
    ttyutils::textbuf::Buffer code{"alpha_beta"};
    assert(ttyutils::wrap::soft_wrap(code.snapshot(), 4, ttyutils::wrap::BreakRules::code()).rows().size() == 1);
    ttyutils::textbuf::Buffer prose{"head\r\na bb\r\nc dd\r\n\r\ntail"};
    ttyutils::wrap::reflow(prose, {{1, 0}, {3, 0}}, 5);
    assert(prose.text() == "head\r\na bb\r\nc dd\r\n\r\ntail");
    ttyutils::wrap::reflow(prose, {{1, 0}, {3, 0}}, 20);
    assert(prose.text() == "head\r\na bb c dd\r\n\r\ntail");
    ttyutils::textbuf::Buffer no_final{"no final"};
    ttyutils::wrap::reflow(no_final, 20);
    assert(no_final.text() == "no final");
    const auto hanging = ttyutils::wrap::soft_wrap(buffer.snapshot(), 4, {true, false, 2});
    const auto continuation = hanging.rows()[1].start;
    assert(hanging.buffer_to_visual(continuation).col == 2);
    assert(hanging.visual_to_buffer({1, 0}) == continuation);
    return 0;
}
