#include "liblr/UTF8Automaton.hpp"
#include "liblr/LexerGenerator.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

using namespace firestone::lr;

namespace {

static_assert(validate_utf8(""));
static_assert(validate_utf8("\xc2\xa2"));
static_assert(!validate_utf8("\xc0\x80"));
static_assert(decode_utf8_prefix("\xf0\x9f\x98\x80!")->code_point == 0x1f600);

void stream() {
    UTF8Automaton decoder;
    CHECK(decoder.status() == UTF8Status::Complete);
    CHECK(!decoder.code_point());
    CHECK(decoder.push(0x00) == UTF8Status::Complete);
    CHECK(decoder.code_point() == 0);
    const unsigned char first_chunk[] = {0xf0, 0x9f};
    for (auto byte : first_chunk) {
        CHECK(decoder.push(byte) == UTF8Status::Incomplete);
        CHECK(!decoder.code_point());
    }
    CHECK(decoder.push(0x98) == UTF8Status::Incomplete);
    CHECK(decoder.push(0x80) == UTF8Status::Complete);
    CHECK(decoder.code_point() == 0x1f600);
    CHECK(decoder.push(0xed) == UTF8Status::Incomplete);
    CHECK(decoder.push(0xa0) == UTF8Status::Invalid);
    CHECK(decoder.push(0x41) == UTF8Status::Invalid);
    CHECK(!decoder.code_point());
    decoder.reset();
    CHECK(decoder.status() == UTF8Status::Complete);
    CHECK(!decoder.code_point());
    CHECK(decoder.push(0xc2) == UTF8Status::Incomplete);
    decoder.reset();
    CHECK(decoder.push(0x41) == UTF8Status::Complete);
    CHECK(decoder.code_point() == 0x41);
}

void errors() {
    const auto dfa = utf8_scalar_dfa();
    auto expect_bad = [&](std::string_view text, std::size_t offset) {
        CHECK(utf8_error_offset(text) == offset);
        CHECK(!validate_utf8(text));
        CHECK(!decode_utf8_prefix(text));
        CHECK(!dfa.match_prefix(text));
    };
    for (const auto text : {"\x80", "\xbf", "\xc0\x80", "\xc1\xbf", "\xf5\x80\x80\x80",
                            "\xf8\x88\x80\x80\x80", "\xff"})
        expect_bad(text, 0);
    for (const auto text : {"\xe0\x9f\xbf", "\xed\xa0\x80", "\xed\xbf\xbf", "\xf0\x8f\xbf\xbf",
                            "\xf4\x90\x80\x80", "\xc2\x41", "\xe1\xc2\xa2"})
        expect_bad(text, 1);
    expect_bad("\xe2\x82\x41", 2);
    expect_bad("\xf1\x80\x80\xff", 3);
    expect_bad("\xc2", 0);
    expect_bad("\xe0\xa0", 0);
    expect_bad("\xf4\x8f\xbf", 0);
    CHECK(utf8_error_offset("a\xf0\x9f\x98\x80\xe2\x82") == 5);
    CHECK(utf8_error_offset("a\xf0\x9f\x98\x80\xe2\x82\xff") == 7);
    CHECK(utf8_error_offset(std::string_view("\0\x80", 2)) == 1);
    CHECK(!decode_utf8_prefix(""));
    CHECK(utf8_error_offset("") == std::nullopt);
    const auto decoded = decode_utf8_prefix("A\xff");
    CHECK(decoded && decoded->length == 1 && decoded->code_point == 0x41);
    CHECK(dfa.match_prefix("\xc2\xa2\xff") == DFAMatch{2, {}});
}

void every_scalar() {
    // Encode from the numeric scalar value, independently of the DFA's
    // byte classification. Check all widths, including U+0000.
    const auto dfa = utf8_scalar_dfa({7, 2, false});
    for (std::uint_least32_t cp = 0; cp <= 0x10ffff; ++cp) {
        if (cp >= 0xd800 && cp <= 0xdfff) continue;
        std::array<char, 4> bytes{};
        const std::size_t length = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        constexpr unsigned int lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
        auto remaining = cp;
        for (auto i = length - 1; i > 0; --i) {
            bytes[i] = static_cast<char>(0x80 | (remaining & 0x3f));
            remaining >>= 6;
        }
        bytes[0] = static_cast<char>(lead[length] | remaining);
        const std::string_view text(bytes.data(), length);
        CHECK(validate_utf8(text));
        const auto decoded = decode_utf8_prefix(text);
        CHECK(decoded && decoded->length == length && decoded->code_point == cp);
        CHECK(dfa.match_prefix(text) == DFAMatch{length, {7, 2, false}});
    }
}

void lexer() {
    const Lexer lexer(utf8_scalar_dfa({23, 0, false}));
    const std::string_view text("A\xc2\xa2\xff\xf0\x9f\x98\x80\0", 9);
    std::size_t offset = 0;
    CHECK(lexer.next(text, offset) == LexToken{LexStatus::Token, 23, 0, 1});
    CHECK(lexer.next(text, offset) == LexToken{LexStatus::Token, 23, 1, 2});
    CHECK(lexer.next(text, offset) == LexToken{LexStatus::Error, 0, 3, 1});
    CHECK(lexer.next(text, offset) == LexToken{LexStatus::Token, 23, 4, 4});
    CHECK(lexer.next(text, offset) == LexToken{LexStatus::Token, 23, 8, 1});
    CHECK(lexer.next(text, offset) == LexToken{LexStatus::End, 0, 9, 0});
    CHECK(offset == text.size());
    // Encoding validity alone does not reject the BOM or noncharacters.
    CHECK(validate_utf8("\xef\xbb\xbf\xef\xbf\xbf\xf4\x8f\xbf\xbf"));
}

} // namespace

int main() {
    try {
        stream();
        errors();
        every_scalar();
        lexer();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
