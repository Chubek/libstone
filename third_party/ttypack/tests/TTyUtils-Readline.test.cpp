#include <TTyUtils/TTyUtils-Readline.hpp>
#include <cassert>

int main() {
    ttyutils::rl::Editor editor;
    editor.feed(ttytk::input::Event{ttytk::input::Key{U'h', 0, 1}});
    editor.feed(ttytk::input::Event{ttytk::input::Key{U'i', 0, 1}});
    editor.feed(ttytk::input::Event{ttytk::input::Key{U'\n', 0, 1}});
    const auto line = ttyutils::rl::read_line(editor);
    assert(line && *line == "hi");

    editor.feed(ttytk::input::Paste{"A\u0301猫"});
    editor.feed(ttytk::input::Key{57350});
    editor.feed(ttytk::input::Key{127});
    assert(editor.value() == "猫");
    assert(editor.undo() && editor.value() == "A\u0301猫");
    editor.feed(ttytk::input::Key{57364}); // F1 must not insert a private-use character.
    editor.feed(ttytk::input::Key{U'a', 0, 3}); // Release.
    assert(editor.value() == "A\u0301猫");

    editor.set_value("abc");
    editor.feed(ttytk::input::Key{U'u', 4});
    assert(editor.value().empty());
    editor.feed(ttytk::input::Key{U'y', 4});
    assert(editor.value() == "abc");
    editor.set_completer([](auto) {
        return std::vector<ttyutils::completion::Candidate>{{"abcdef"}, {"abcedf"}};
    });
    editor.feed(ttytk::input::Key{U'\t'});
    assert(editor.value() == "abc");
    editor.set_multiline(true);
    editor.set_value("(");
    editor.feed(ttytk::input::Key{U'\r'});
    assert(editor.value() == "(\n" && !editor.read());
    editor.feed(ttytk::input::Key{U')'});
    editor.feed(ttytk::input::Key{U'\r'});
    assert(editor.read() == "(\n)");
    return 0;
}
