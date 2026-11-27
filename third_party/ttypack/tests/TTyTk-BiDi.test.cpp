#include <TTyTk/TTyTk-BiDi.hpp>

#include <cassert>

int main() {
    using namespace ttytk::bidi;
    using Levels = std::vector<Level>;
    const auto text = [](std::u32string_view scalars) {
        return ttytk::utf8::encode(ttytk::Span<const char32_t>{scalars.data(), scalars.size()});
    };
    const auto visual = [](const std::string& input) { return reorder_line(input, resolve(input)); };
    assert(base_direction("abc") == 0);
    assert(base_direction(text(U"_ \u05D0")) == 1);
    assert(base_direction(text(U"\u00E9 \u05D0")) == 0);
    assert(base_direction(text(U"\u2067\u05D0\u2069 abc")) == 0);
    assert(base_direction(text(U"\u0661 \u05D0")) == 1);

    assert((resolve(text(U"abc \u05D0\u05D1 123")) == Levels{0, 0, 0, 0, 1, 1, 1, 2, 2, 2}));
    assert((resolve(text(U"\u05D0\nabc")) == Levels{1, 1, 0, 0, 0}));
    assert(visual(text(U"abc \u05D0\u05D1 123")) == text(U"abc 123 \u05D1\u05D0"));
    assert(visual(text(U"\u05D0(12)\u05D1")) == text(U"\u05D1(12)\u05D0"));
    assert(visual(text(U"\u05D0\u05B7\u05D1")) == text(U"\u05D1\u05D0\u05B7"));
    assert(visual(text(U"abc \u202E123\u202C")) == "abc 321");
    assert(visual(text(U"L\u2067\u05D0\u05D1\u2069R")) == text(U"L\u05D1\u05D0R"));
    assert(visual(text(U"L\u2068\u05D0\u05D1\u2069R")) == text(U"L\u05D1\u05D0R"));
    const auto emoji = text(U"\U0001F469\u200D\U0001F4BB");
    assert(visual(emoji) == emoji);
    assert(visual(text(U"\u05D0\u05D1 ") + emoji) == emoji + text(U" \u05D1\u05D0"));
    const auto skin_tone = text(U"\U0001F44D\U0001F3FD");
    assert(visual(text(U"\u05D0 ") + skin_tone) == skin_tone + text(U" \u05D0"));
    assert(visual("\xF0\x9F" "X") == "\xEF\xBF\xBD" "X");
    assert(reorder_line("ab", Levels{1}) == "ab");
    assert(reorder_line("abc12", Levels{0, 1, 1, 2, 2}) == "a12cb");
}
