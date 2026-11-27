#include <TTyTk/TTyTk-Scrollback.hpp>
#include <cassert>

int main() {
    ttytk::Scrollback history;
    history.push_line({{"A"}, {"B"}}, true);
    ttytk::CellBuffer row{1, 4};
    row.put(0, 0, {"\xE5\xAD\x97"});
    row.put(0, 2, {"C", 1});
    history.push_line({row.at(0, 0), row.at(0, 1), row.at(0, 2), row.at(0, 3)});
    assert(history.line(1).size() == 4);
    history.push_line({{"D"}});
    history.reflow(3);
    assert(history.size() == 3);
    assert(history.line(0)[1].grapheme == "B" && history.line(0)[2] == ttytk::Cell{});
    assert(history.line(1)[1].continuation && history.line(1)[2].attributes == 1);
    history.reflow(6); // neither the old blank nor the new padding became content
    assert(history.size() == 2);
    assert(history.line(0)[3].continuation && history.line(0)[4].grapheme == "C");
    assert(history.line(1).front().grapheme == "D");
    history.reflow(1);
    assert(history.size() == 5 && history.line(2).size() == 2 && history.line(2)[1].continuation);
    history.set_limit(1);
    assert(history.size() == 1 && history.line(0).front().grapheme == "D");
    history.set_limit(0);
    history.push_line({{"x"}});
    assert(history.size() == 0);
}
