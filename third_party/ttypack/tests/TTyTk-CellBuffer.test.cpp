#include <TTyTk/TTyTk-CellBuffer.hpp>

#include <cassert>
#include <limits>
#include <stdexcept>

int main() {
    ttytk::CellBuffer buffer{3, 5};
    constexpr auto wide = "\xE5\xAD\x97";
    buffer.put(0, 0, {wide});
    buffer.put(0, 2, {"a"}); // neighbor is not overlapping
    assert(buffer.at(0, 1).continuation);
    buffer.put(0, 1, {"b"});
    assert(buffer.at(0, 0).grapheme == " ");
    assert(buffer.at(0, 2).grapheme == "a");

    buffer.put(0, 2, {wide});
    buffer.put(0, 1, {wide, 2});
    assert(buffer.at(0, 2).continuation && buffer.at(0, 2).attributes == 2);
    assert(buffer.at(0, 3) == ttytk::Cell{});
    buffer.scroll(0, 2, -1);
    assert(buffer.at(1, 2).continuation);
    buffer.resize(3, 2);
    assert(buffer.at(1, 1) == ttytk::Cell{});
    buffer.put(1, 1, {wide});
    assert(buffer.at(1, 1).grapheme == " ");

    buffer.resize(3, 4);
    buffer.put(1, 0, {"\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"});
    assert(buffer.at(1, 1).continuation);
    buffer.put(1, 2, {"e\xCC\x81"});
    assert(!buffer.at(1, 3).continuation);
    buffer.put(2, 0, {"x"});
    buffer.scroll(0, 2, std::numeric_limits<int>::min());
    assert(buffer.at(1, 0).grapheme == " ");
    assert(buffer.at(2, 0).grapheme == "x");

    bool invalid = false;
    try { (void)buffer.at(0, 4); }
    catch (const std::out_of_range&) { invalid = true; }
    assert(invalid);
    invalid = false;
    try { buffer.resize(std::numeric_limits<std::size_t>::max() / 2 + 1, 2); }
    catch (const std::length_error&) { invalid = true; }
    assert(invalid && buffer.rows() == 3 && buffer.columns() == 4);
}
