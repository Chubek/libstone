#include <TTyUtils/TTyUtils-TextCursor.hpp>
#include <cassert>

int main() {
    ttyutils::textbuf::Buffer buffer{"hello world"};
    ttyutils::textcur::CursorSet cursors{buffer};
    cursors.add({0, 0});
    cursors.move(ttyutils::textcur::Motion::word_right);
    assert(cursors.cursors().front().position().col == 5);
    cursors.insert_each("!");
    assert(buffer.text() == "hello! world");
    using namespace ttyutils::textcur;
    ttyutils::textbuf::Buffer document{"long\r\nx\r\nlonger"};
    Cursor sticky{Position{0, 4}};
    sticky.move(document, Motion::down);
    assert((sticky.position() == Position{1, 1}));
    sticky.move(document, Motion::down);
    assert((sticky.position() == Position{2, 4}));
    assert(sticky.selection().begin() == sticky.selection().end());
    sticky.move(document, Motion::left, true);
    assert((sticky.selection().anchor == Position{2, 4}));
    ttyutils::textbuf::Buffer edits{"abc def"};
    CursorSet multi{edits};
    multi.add({0, 1});
    multi.add({0, 5});
    multi.insert_each("\n!");
    assert(edits.text() == "a\n!bc d\n!ef");
    assert((multi.cursors()[0].position() == Position{1, 1}));
    assert((multi.cursors()[1].position() == Position{2, 1}));
    edits.erase({0, 0}, {1, 1});
    assert((multi.cursors()[0].position() == Position{0, 0}));
    assert((multi.cursors()[1].position() == Position{1, 1}));
    ttyutils::textbuf::Buffer selections{"abcd"};
    CursorSet selected{selections};
    selected.add_selection({{0, 1}, {0, 3}});
    selected.add_selection({{0, 2}, {0, 4}});
    assert(selected.cursors().size() == 1);
    selected.insert_each("!");
    assert(selections.text() == "a!");
    ttyutils::textbuf::Buffer semantic{"é HTTPServer42(a[b])"};
    Cursor semantic_cursor;
    semantic_cursor.move(semantic, Motion::word_right);
    assert(semantic_cursor.position().col == 1);
    semantic_cursor.move(semantic, Motion::subword_right);
    semantic_cursor.move(semantic, Motion::subword_right);
    assert(semantic_cursor.position().col == 6);
    semantic_cursor.move(semantic, Motion::subword_right);
    semantic_cursor.move(semantic, Motion::subword_right);
    assert(semantic_cursor.position().col == 14);
    semantic_cursor.move(semantic, Motion::matching_bracket);
    assert(semantic_cursor.position().col == 19);
    return 0;
}
