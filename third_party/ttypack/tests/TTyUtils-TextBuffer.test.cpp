#include <TTyUtils/TTyUtils-TextBuffer.hpp>
#include <cassert>

int main() {
    ttyutils::textbuf::Buffer buffer{"one\né\n"};
    assert(buffer.line_index().lines() == 3);
    buffer.insert({1, 1}, "x");
    assert(buffer.line(1) == "éx");
    auto snapshot = buffer.snapshot();
    buffer.insert({0, 0}, "!");
    assert(snapshot.text() == "one\néx\n");
    assert(buffer.text().starts_with('!'));
    using ttyutils::textbuf::Buffer;
    Buffer windows{"\xEF\xBB\xBF" "e\xCC\x81\r\nx\r\n"};
    assert(windows.has_bom() && windows.eol() == Buffer::Eol::crlf);
    assert(windows.line(0) == "e\xCC\x81");
    const auto index = windows.line_index();
    assert(index.offset({0, 1}, windows.text()) == 6);
    assert((index.position(5, windows.text()) == Buffer::Position{0, 0}));
    assert((index.position(7, windows.text()) == Buffer::Position{0, 1}));
    windows.insert({0, 200}, "!");
    assert(windows.text() == "\xEF\xBB\xBF" "e\xCC\x81!\r\nx\r\n");
    const auto before = windows.snapshot();
    windows.replace({0, 0}, {1, 0}, windows.line(1));
    assert(windows.text() == "\xEF\xBB\xBF" "xx\r\n");
    assert(before.line(0) == "e\xCC\x81!");
    windows.insert({0, 1}, "\n");
    assert(windows.eol() == Buffer::Eol::mixed);
    const auto revision = windows.generation();
    windows.insert({0, 0}, "");
    assert(windows.generation() == revision);
    return 0;
}
