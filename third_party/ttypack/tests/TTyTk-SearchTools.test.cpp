#include <TTyTk/TTyTk-SearchTools.hpp>
#include <cassert>
int main() {
    using namespace ttytk;
    CellBuffer buffer{2, 5};
    buffer.put(0, 0, {"界", 1});
    buffer.put(0, 2, {"a"});
    buffer.put(0, 3, {"é"});
    buffer.put(0, 4, {"a"});
    buffer.put(1, 0, {"A"});
    assert(!search::find(buffer, "a\nA"));
    const auto all = search::find_all(buffer, "A", true);
    assert(all.size() == 3 && all[0].column == 2 && all[1].column == 4 && all[2].row == 1);
    const auto match = search::find(buffer, "界aé");
    assert(match && match->row == 0 && match->column == 0 && match->length == 4);
    assert(search::find(buffer, search::Regex{"éa"})->column == 3);
    assert(search::find_all(buffer, search::Regex{"^"}).size() == 2);
    const auto styled = search::find_all(buffer, 1);
    assert(styled.size() == 1 && styled[0].length == 2);

    Scrollback history;
    history.push_line({{"x"}});
    history.push_line({{"a"}, {"a"}, {"a"}});
    const auto overlapping = search::find_all(history, "aa");
    assert(overlapping.size() == 2 && overlapping[0].row == 1 && overlapping[1].column == 1);
}
