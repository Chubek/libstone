#include <TTyUtils/TTyUtils-Terminal.hpp>
#include <cassert>
#include <chrono>

int main() {
    ttyutils::term::Emulator emulator{2, 5};
    std::string osc_code;
    emulator.on_osc([&](std::string code, std::string) { osc_code = std::move(code); });
    emulator.feed("hello\r\nworld\x1b]2;title\a");
    assert(emulator.screen()[0] == "hello");
    assert(emulator.screen()[1] == "world");
    assert(osc_code == "2");
    emulator.feed("\x1b[");
    emulator.feed("1;2HZ\x1b[1");
    emulator.feed("K");
    assert(emulator.screen()[0] == "   lo");
    emulator.feed("\x1b[2;1H\x1b[38;2;9;8;7m\xE7");
    emulator.feed("\x95\x8C");
    assert(emulator.cells().at(1, 0).grapheme == "界");
    emulator.feed("\x1b[12\x18\x1b[1;5Hq");
    assert(emulator.screen()[0].back() == 'q');
    emulator.feed("\x1b]52;c;ZGVuaWVk\x1b");
    assert(osc_code == "2");
    emulator.feed("\\");
    assert(osc_code == "52");
    assert(emulator.cells().at(1, 1).continuation);
    assert((emulator.cells().at(1, 0).foreground == ttytk::color::Rgb{9, 8, 7}));
    emulator.feed("\x1b[?1049htemp");
    assert(emulator.screen()[0] == "temp ");
    emulator.feed("\x1b[?1049l");
    assert(emulator.cells().at(1, 0).grapheme == "界");
    emulator.feed("\x1b[?1000h");
    assert(emulator.mouse_reporting());
    emulator.feed("\x1b[?1000l");
    assert(!emulator.mouse_reporting());
    emulator.resize(0, 0);
    emulator.feed("ignored\n\x1b[2J");
    assert(emulator.screen().empty());

    ttyutils::term::Emulator wrap{2, 3};
    wrap.feed("abcdefg");
    assert(wrap.screen()[0] == "def");
    assert(wrap.screen()[1] == "g  ");
    assert(wrap.scrollback().lines().back() == "abc");
    wrap.feed("\x1b[3J");
    assert(wrap.scrollback().lines().empty());

    ttyutils::loop::Loop loop;
    ttyutils::term::Terminal terminal;
    assert(terminal.resize({7, 16}));
    std::string title, cwd;
    terminal.on_title([&](std::string value) { title = std::move(value); });
    terminal.on_cwd([&](std::string value) { cwd = std::move(value); });
    assert(terminal.spawn({"/bin/sh", "-c",
        "stty -echo; printf '\\033]2;test title\\007\\033]7;file:///somewhere\\033\\\\'; read -r line; stty size"}));
    terminal.attach(loop);
    for (int i = 0; i < 50 && (title.empty() || cwd.empty()); ++i)
        loop.run_once(std::chrono::milliseconds(10));
    assert(title == "test title" && cwd == "file:///somewhere");
    assert(terminal.write("go\n"));
    for (int i = 0; i < 50 && !terminal.emulator().screen()[0].starts_with("7 16"); ++i)
        loop.run_once(std::chrono::milliseconds(10));
    assert(terminal.emulator().screen()[0].starts_with("7 16"));
    terminal.close();
    loop.run_once(std::chrono::milliseconds(0));
    return 0;
}
