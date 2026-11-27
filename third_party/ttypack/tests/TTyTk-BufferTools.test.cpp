#include <TTyTk/TTyTk-BufferTools.hpp>
#include <cassert>
int main() {
    using namespace ttytk;
    CellBuffer buffer{1, 4};
    buffer.put(0, 0, {"A"});
    buffer.put(0, 1, {"界"});
    assert(bufutil::dump_text(buffer) == "A界 ");
    assert(bufutil::compare(buffer, buffer));
    assert(bufutil::diff(buffer, buffer).empty());
    auto styled = buffer;
    styled.at(0, 0).attributes = static_cast<std::uint32_t>(CellAttribute::Bold);
    styled.at(0, 0).foreground = {1, 2, 3};
    assert(bufutil::dump_text(styled) == bufutil::dump_text(buffer));
    assert(!bufutil::compare(buffer, styled));
    const auto delta = bufutil::diff(buffer, styled);
    assert(delta == "\x1b[1;1H\x1b[0;1;38;2;1;2;3;48;2;0;0;0mA\x1b[0m");
    assert(bufutil::dump_ansi(styled).find("界 ") != std::string::npos);
    assert(!bufutil::compare(CellBuffer{0, 1}, CellBuffer{0, 2}));
    assert(bufutil::diff(buffer, CellBuffer{}) == "\x1b[2J");
    auto shrunk = buffer;
    shrunk.put(0, 1, {"B"});
    assert(bufutil::diff(buffer, shrunk).find("\x1b[1;3H") != std::string::npos);
}
