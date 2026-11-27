#include <TTyUtils/TTyUtils-Pager.hpp>
#include <cassert>
#include <sstream>

int main() {
    std::istringstream stream{"first\n\x1b[38;2;1;2;3mred\nstill red\x1b[0m\nlast"};
    auto pager = ttyutils::pager::page_stream(stream);
    assert(pager.poll(11).bytes == 11);
    assert(pager.length() == 2); // The first chunk ends inside the CSI sequence.
    pager.follow(true);
    assert(pager.poll().bytes > 0);
    assert((pager.lines() == std::vector<std::string_view>{"first", "red", "still red", "last"}));
    pager.set_rect({0, 0, 12, 2});
    pager.scroll_to(1);
    assert(pager.offset() == 1 && !pager.following());
    ttyutils::ui::Canvas canvas{{0, 0, 12, 2}};
    pager.render(canvas);
    assert(canvas.texts().size() == 2);
    const ttytk::color::Rgb expected{1, 2, 3};
    assert(canvas.texts()[0].style.foreground == expected);
    assert(canvas.texts()[1].style.foreground == expected);

    pager.set_marks({{0}});
    assert(pager.jump_mark(0) && pager.offset() == 0);
    assert(pager.find(ttyutils::search::Query::literal("last")) == 3);
    pager.follow(true);
    pager.append(" line\nnew");
    assert(pager.lines().back() == "new");
    assert(pager.offset() == 3);

    ttyutils::pager::Pager emoji{ttyutils::pager::Source{"👩‍👩‍👧‍👦!"}};
    emoji.set_rect({0, 0, 2, 2});
    emoji.set_wrap(true);
    ttyutils::ui::Canvas wrapped{{0, 0, 2, 2}};
    emoji.render(wrapped);
    assert(wrapped.texts().size() == 2 && wrapped.texts().back().value == "!");
}
