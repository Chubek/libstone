#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-OSCTools.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace ttytk::notify {

namespace detail {

[[nodiscard]] inline auto field(const std::string_view text, const bool title = false)
    -> std::string {
    std::string result;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto byte = static_cast<unsigned char>(text[offset]);
        // Keep whole UTF-8 characters, but not Unicode C1 controls or bare C1
        // bytes (e.g. ST). Do not confuse continuation bytes with controls.
        std::size_t length = byte >= 0xC2 && byte <= 0xDF ? 2 :
                             byte >= 0xE0 && byte <= 0xEF ? 3 :
                             byte >= 0xF0 && byte <= 0xF4 ? 4 : 1;
        if (length > text.size() - offset) length = 1;
        for (std::size_t index = 1; index < length; ++index) {
            if ((static_cast<unsigned char>(text[offset + index]) & 0xC0U) != 0x80U) length = 1;
        }
        if (length > 1 && !(byte == 0xC2 &&
                            static_cast<unsigned char>(text[offset + 1]) <= 0x9F)) {
            result.append(text.substr(offset, length));
        } else if (byte >= 0x20 && byte < 0x7F) {
            result += title && byte == ';' ? ' ' : static_cast<char>(byte);
        }
        offset += length;
    }
    return result;
}

}  // namespace detail

/** Build a WezTerm/rxvt OSC 777 notification with plain-text fields. */
[[nodiscard]] inline auto show(const std::string_view title, const std::string_view body = {})
    -> std::string {
    return "\x1b]777;notify;" + detail::field(title, true) + ';' + detail::field(body) + "\x1b\\";
}

/** Build an OSC 9;4 progress report for compatible terminals. */
[[nodiscard]] inline auto progress(const unsigned value, const unsigned total) -> std::string {
    const auto percent = total == 0 ? 0U :
                         value >= total ? 100U : (std::uint64_t{value} * 100U) / total;
    return "\x1b]9;4;1;" + std::to_string(percent) + "\x1b\\";
}

/** Build an iTerm2 badge command, encoding the supplied text as Base64. */
[[nodiscard]] inline auto badge(const std::string_view text) -> std::string {
    constexpr std::string_view alphabet{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    std::string output{"\x1b]1337;SetBadgeFormat="};
    for (std::size_t offset = 0; offset < text.size();) {
        const auto remaining = text.size() - offset;
        const auto first = static_cast<unsigned char>(text[offset++]);
        const auto second = remaining > 1 ? static_cast<unsigned char>(text[offset++]) : 0U;
        const auto third = remaining > 2 ? static_cast<unsigned char>(text[offset++]) : 0U;
        output += alphabet[first >> 2U];
        output += alphabet[((first & 3U) << 4U) | (second >> 4U)];
        output += remaining > 1 ? alphabet[((second & 15U) << 2U) | (third >> 6U)] : '=';
        output += remaining > 2 ? alphabet[third & 63U] : '=';
    }
    output += '\x07';
    return output;
}

}  // namespace ttytk::notify
