#include <TTyTk/TTyTk-Grampheme.hpp>

#include <cassert>

int main() {
    assert(ttytk::grapheme::count("a\xCC\x81" "b") == 2);
    assert(ttytk::grapheme::count("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8") == 1);
    assert(ttytk::grapheme::count("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB") == 1);
    const auto segments = ttytk::grapheme::segment("a\xCC\x81" "b");
    assert(segments.size() == 2);
    assert(segments[0] == "a\xCC\x81");
    assert(ttytk::grapheme::next_break("ab") == 1);
    using ttytk::grapheme::count;
    using ttytk::utf8::encode;
    const auto text = [&](std::u32string_view scalars) {
        return encode(ttytk::Span<const char32_t>{scalars.data(), scalars.size()});
    };
    assert(count(text(U"\U0001F44D\U0001F3FD")) == 1);
    assert(count(text(U"\u0915\u094D\u0937")) == 1);
    assert(count(text(U"\u1100\u1161\u11A8")) == 1);
    assert(count(text(U"\U0001F469\u200D\u200D\U0001F4BB")) == 2);
    assert(count(text(U"\U0001F1FA\U0001F1F8\U0001F1E8")) == 2);
    assert(count(text(U"\U0001F3F4\U000E0067\U000E0062\U000E007F")) == 1);
    assert(count("a\r\nb") == 3);
    assert(ttytk::grapheme::next_break("\xE2\x82" "X") == 2);

    const std::string_view iterated = "a\xCC\x81" "b";
    ttytk::grapheme::Iterator it(iterated, 0), end(iterated, iterated.size());
    static_assert(std::forward_iterator<decltype(it)>);
    assert(*it++ == "a\xCC\x81");
    assert(*it++ == "b");
    assert(it == end);
}
