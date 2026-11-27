#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <initializer_list>
#include <string>
#include <string_view>

namespace ttytk::esc {

/** @brief ECMA-48 7-bit Control Sequence Introducer. */
inline constexpr std::string_view csi{"\x1B["};
/** @brief ECMA-48 7-bit Operating System Command introducer. */
inline constexpr std::string_view osc{"\x1B]"};
/** @brief ECMA-48 7-bit Device Control String introducer. */
inline constexpr std::string_view dcs{"\x1BP"};
/** @brief ECMA-48 7-bit String Terminator. */
inline constexpr std::string_view st{"\x1B\\"};

/** @brief Basic ECMA-48 Select Graphic Rendition parameters. */
enum Sgr : unsigned {
    Reset = 0,
    Bold = 1,
    Faint = 2,
    Italic = 3,
    Underline = 4,
    Blink = 5,
    Inverse = 7,
    Conceal = 8,
    Strike = 9,
};

[[nodiscard]] inline auto sgr(const std::initializer_list<unsigned> parameters) -> std::string {
    std::string result{csi};
    bool first = true;
    for (const auto parameter : parameters) {
        if (!first) result.push_back(';');
        result += std::to_string(parameter);
        first = false;
    }
    result.push_back('m');
    return result;
}

[[nodiscard]] inline auto sgr(const Sgr parameter) -> std::string {
    return sgr({static_cast<unsigned>(parameter)});
}

/** @brief Build ECMA-48 CUP; coordinates are one-based and zero means 1. */
[[nodiscard]] inline auto cursor_to(const unsigned row, const unsigned column) -> std::string {
    return std::string{csi} + std::to_string(row == 0 ? 1 : row) + ';' +
           std::to_string(column == 0 ? 1 : column) + 'H';
}

/** @brief Erase the complete display (ED 2), without moving the cursor. */
[[nodiscard]] inline auto clear() -> std::string {
    return std::string{csi} + "2J";
}

namespace detail {

[[nodiscard]] inline auto printable_utf8(const std::string_view text) -> std::string {
    std::string result;
    result.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = ttytk::detail::decode_utf8_scalar(text, offset);
        if (scalar.valid && scalar.value >= 0x20U &&
            !(scalar.value >= 0x7FU && scalar.value <= 0x9FU)) {
            result += text.substr(offset, scalar.length);
        }
        offset += scalar.length;
    }
    return result;
}

}  // namespace detail

/** @brief Build OSC 8 hyperlink delimiters; URIs must be control-free UTF-8. */
struct Hyperlink {
    [[nodiscard]] static inline auto open(const std::string_view uri) -> std::string {
        const auto clean = detail::printable_utf8(uri);
        if (clean != uri) return close();
        return std::string{osc} + "8;;" + clean + std::string{st};
    }
    [[nodiscard]] static inline auto close() -> std::string {
        return std::string{osc} + "8;;" + std::string{st};
    }
};

}  // namespace ttytk::esc

// NOTE(agent): TTyUtils manifests name TTyTk::Hyperlink; keep a global alias
// to the escape-code-owned builder without adding a new header.
namespace ttytk {
using Hyperlink = esc::Hyperlink;
}  // namespace ttytk
