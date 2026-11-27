#include <TTyTk/TTyTk-LineEdit.hpp>
#include <cassert>

int main() {
    ttytk::Editor editor;
    editor.insert(U'é');
    editor.insert(U'\u0301');
    editor.insert(U'界');
    editor.move_left();
    editor.backspace();
    assert(editor.line() == "界" && editor.cursor() == 0);
    editor.move_right();
    editor.insert(U'\n');
    editor.insert(U'b');
    editor.move_left();
    editor.kill_to_end();
    assert(editor.line() == "界\n");
    editor.yank();
    assert(editor.line() == "界\nb");
    editor.on_submit([&](std::string_view line) {
        assert(line == "界\nb");
        editor.insert(U'd');
    });
    editor.submit();
    editor.move_left();
    assert(editor.history_up() == "界\nb");
    assert(editor.history_down() == "d" && editor.cursor() == 0);

    editor.set_completion_callback([called = 0](std::string_view text) mutable {
        assert(text == "d");
        if (++called == 1) return std::vector<std::string>{"draft", "done"};
        return std::vector<std::string>{"done"};
    });
    assert(editor.complete().size() == 2 && editor.line() == "d");
    assert(editor.complete().size() == 1 && editor.line() == "done");
}
