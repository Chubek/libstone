#include <TTyTk/TTyTk-CharWidth.hpp>

#include <cassert>

int main() {
    assert(ttytk::charwidth::of(U'A') == 1);
    assert(ttytk::charwidth::of(U'\u0301') == 0);
    assert(ttytk::charwidth::of(U'\u5B57') == 2);
    assert(ttytk::charwidth::is_wide(U'\U0001F600'));
    assert(ttytk::charwidth::is_zero(U'\u200D'));
    assert(ttytk::charwidth::of_string("A\xCC\x81\xE5\xAD\x97") == 3);
    assert(ttytk::charwidth::of(U'\u00B7', true) == 2);
    assert(ttytk::charwidth::of(U'\u00A7', false) == 1);
    assert(ttytk::charwidth::of(U'\u2212', true) == 1);
    assert(ttytk::charwidth::of(U'\uE000', true) == 2);
    assert(ttytk::charwidth::of(U'\U0001F700') == 1);
    assert(ttytk::charwidth::of_string("\xE2\x82" "A") == 2);
    assert(ttytk::charwidth::of_string("\x80" "A") == 2);
    assert(ttytk::charwidth::of_grapheme("\xC2\xA9\xEF\xB8\x8F") == 2);
    assert(ttytk::charwidth::of_grapheme("1\xEF\xB8\x8F\xE2\x83\xA3") == 2);
    assert(ttytk::charwidth::of_grapheme("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8") == 2);
    assert(ttytk::charwidth::of_grapheme("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB") == 2);
}
