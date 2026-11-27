#include <TTyUtils/TTyUtils-UndoHistory.hpp>
#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    ttyutils::textbuf::Buffer buffer{"a"};
    ttyutils::undo::History history{buffer};
    { auto transaction = history.transaction(); buffer.insert({0, 1}, "b"); }
    assert(buffer.text() == "ab");
    assert(history.undo());
    assert(buffer.text() == "a");
    assert(history.redo_branch());
    assert(buffer.text() == "ab");
    namespace text = ttyutils::textbuf;
    namespace cursor = ttyutils::textcur;
    namespace undo = ttyutils::undo;
    text::Buffer document{"\xEF\xBB\xBF" "first\r\n"};
    cursor::CursorSet set{document};
    set.add({0, 5});
    undo::History tree{document, set};
    { auto type = tree.typing_transaction(); set.insert_each("x"); }
    { auto type = tree.typing_transaction(); set.insert_each("y"); }
    assert(tree.nodes().size() == 2);
    assert(tree.undo() && document.text() == "\xEF\xBB\xBF" "first\r\n");
    assert((set.cursors()[0].position() == text::Buffer::Position{0, 5}));
    { auto action = tree.transaction(); set.insert_each(" \"other\\"); }
    assert(tree.undo());
    tree.checkpoint("before \"format\"");
    const auto path = std::filesystem::temp_directory_path() / "ttypack-UndoHistory.history";
    assert(tree.save(path));
    undo::History loaded{document, set};
    assert(loaded.load(path));
    assert(loaded.checkpoints()[0].name == "before \"format\"");
    assert(loaded.redo_branch(1) && document.text() == "\xEF\xBB\xBF" "first \"other\\\r\n");
    assert(loaded.undo());
    assert(loaded.redo_branch(0) && document.text() == "\xEF\xBB\xBF" "firstxy\r\n");
    assert(set.cursors()[0].position().col == 7);
    std::ofstream{path, std::ios::trunc} << "TTyUtils-undo-1\n1 0 0\n7 \"\" \"\" \"\" 0 0";
    assert(!loaded.load(path) && loaded.can_undo());
    std::filesystem::remove(path);
    return 0;
}
