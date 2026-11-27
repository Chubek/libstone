#include <TTyTk.hpp>
#include <TTyUtils.hpp>

#include <cassert>
#include <sstream>
#include <string>

int main() {
    ttytk::VirtualTerm terminal{2, 8};
    terminal.input("status\r\n");
    terminal.input("ready\xf0\x9f");
    terminal.input("\x98\x80");
    assert(terminal.screen().at(1, 5).grapheme == "\xf0\x9f\x98\x80");
    assert(terminal.screen().at(1, 6).continuation);

    const ttyutils::textbuf::Buffer transcript{
        ttytk::bufutil::dump_text(terminal.screen())};
    const auto snapshot = transcript.snapshot();
    assert(snapshot.text().starts_with("status"));
    auto matches = ttyutils::search::find_all(
        snapshot, ttyutils::search::Query::literal("ready"));
    assert(matches.size() == 1);

    ttytk::Renderer renderer;
    std::ostringstream display;
    renderer.render(terminal.screen(), display);
    assert(display.str().find("ready") != std::string::npos);
}
