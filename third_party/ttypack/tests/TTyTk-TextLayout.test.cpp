#include <TTyTk/TTyTk-TextLayout.hpp>
#include <cassert>
#include <limits>

int main() {
    using namespace ttytk::layout;
    using Lines = std::vector<std::string>;
    assert(measure("abc") == 3);
    assert((wrap("abcd", 2) == Lines{"ab", "cd"}));
    assert((wrap("one  two three", 7) == Lines{"one", "two", "three"}));
    assert((wrap("one  two", 8) == Lines{"one  two"}));
    assert((wrap("word  ", 4) == Lines{"word"}));
    assert((wrap("one\xE2\x80\x83two", 4) == Lines{"one", "two"}));  // EM SPACE
    assert((wrap("a\xC2\xA0" "bc", 3) == Lines{"a\xC2\xA0" "b", "c"}));  // NBSP
    assert((wrap("a\r\n\nb\n", 0) == Lines{"a", "", "b", ""}));
    assert((wrap("a b", std::numeric_limits<std::size_t>::max()) == Lines{"a b"}));
    assert((reflow("one two", 4) == Lines{"one", "two"}));
    const std::string emoji = "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";
    assert(measure(emoji + "\nabc") == 3);
    assert((wrap(emoji + "x", 1) == Lines{emoji, "x"}));
    assert(truncate(emoji + "ab", 3) == emoji + "\xE2\x80\xA6");
    assert(truncate(emoji + "x", 2) == "\xE2\x80\xA6");
    assert(truncate("a\nb", 2) == "a\xE2\x80\xA6");
    assert(truncate("a\xCC\x81" "bc", 2) == "a\xCC\x81\xE2\x80\xA6");
}
