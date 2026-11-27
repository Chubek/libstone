#include <TTyTk/TTyTk-Snapshot.hpp>
#include <cassert>

int main() {
    namespace snapshot = ttytk::snapshot;
    ttytk::CellBuffer buffer{1, 2};
    buffer.put(0, 0, {"\xE5\xAD\x97", 1, {1, 2, 3}, {4, 5, 6}});
    const auto state = snapshot::capture(buffer);
    assert(snapshot::diff(buffer, state).empty());
    assert(snapshot::serialize(state) ==
           "TTyTk snapshot v1 1 2\n- 1 010203 040506 e5ad97\n+ 1 010203 040506 \n");
    buffer.at(0, 1).background.red = 9;
    assert(snapshot::diff(state, buffer) == "cell differs at (0, 1)");
    assert(snapshot::serialize(state) != snapshot::serialize(buffer));
    buffer.resize(1, 1);
    assert(snapshot::diff(buffer, state) == "size differs");
    assert(state.columns() == 2 && state.at(0, 1).continuation);
}
