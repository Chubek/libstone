#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>

#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ttytk::input {

/** @brief A decoded keyboard report with Kitty modifier bits and optional alternate keys. */
struct Key {
    char32_t codepoint{};
    unsigned modifiers{};
    /** @brief 1 for press, 2 for repeat, 3 for release. */
    unsigned event_type{1};
    std::optional<char32_t> shifted{};
    std::optional<char32_t> base_layout{};
    std::u32string text{};
};

struct Mouse {
    /** @brief SGR button code without modifiers or motion (3 means no button for motion). */
    unsigned button{};
    /** @brief SGR modifier bits: Shift=4, Meta=8, Control=16. */
    unsigned modifiers{};
    /** @brief One-based column in SGR wire coordinates. */
    unsigned x{};
    /** @brief One-based row in SGR wire coordinates. */
    unsigned y{};
    /** @brief A button is pressed, including during a drag; false on release or hover. */
    bool pressed{};
    bool motion{};
};

struct Paste {
    std::string text{};
};

using Event = std::variant<Key, Mouse, Paste>;

namespace detail {

inline constexpr std::string_view paste_start{"\x1B[200~"};
inline constexpr std::string_view paste_end{"\x1B[201~"};
inline constexpr unsigned mouse_button_mask = 3U | 64U | 128U;
inline constexpr unsigned mouse_modifier_mask = 4U | 8U | 16U;

[[nodiscard]] inline auto number(const std::string_view text, unsigned& value) -> bool {
    if (text.empty()) return false;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), value);
    return converted.ec == std::errc{} && converted.ptr == text.data() + text.size();
}

[[nodiscard]] inline auto take(std::string_view& text, const char delimiter) -> std::string_view {
    const auto separator = text.find(delimiter);
    const auto first = text.substr(0, separator);
    text = separator == std::string_view::npos ? std::string_view{} : text.substr(separator + 1);
    return first;
}

[[nodiscard]] inline auto scalar(const char32_t value) -> bool {
    return value <= 0x10FFFFU && !(value >= 0xD800U && value <= 0xDFFFU);
}

[[nodiscard]] inline auto codepoint(const std::string_view text, char32_t& value) -> bool {
    unsigned parsed{};
    if (!number(text, parsed) || !scalar(static_cast<char32_t>(parsed))) return false;
    value = static_cast<char32_t>(parsed);
    return true;
}

[[nodiscard]] inline auto text_codepoint(const char32_t value) -> bool {
    return scalar(value) && value >= 0x20U && !(value >= 0x7FU && value <= 0x9FU);
}

struct FunctionalKey {
    char32_t code;
    unsigned parameter;
    char final;
};

// Kitty's legacy functional wire forms. Private-use values are the normalized
// key identities exposed to applications regardless of the wire spelling.
inline constexpr std::array functional_keys{
    FunctionalKey{57348, 2, '~'}, FunctionalKey{57349, 3, '~'},
    FunctionalKey{57350, 1, 'D'}, FunctionalKey{57351, 1, 'C'},
    FunctionalKey{57352, 1, 'A'}, FunctionalKey{57353, 1, 'B'},
    FunctionalKey{57354, 5, '~'}, FunctionalKey{57355, 6, '~'},
    FunctionalKey{57356, 1, 'H'}, FunctionalKey{57357, 1, 'F'},
    FunctionalKey{57364, 1, 'P'}, FunctionalKey{57365, 1, 'Q'},
    FunctionalKey{57366, 13, '~'}, FunctionalKey{57367, 1, 'S'},
    FunctionalKey{57368, 15, '~'}, FunctionalKey{57369, 17, '~'},
    FunctionalKey{57370, 18, '~'}, FunctionalKey{57371, 19, '~'},
    FunctionalKey{57372, 20, '~'}, FunctionalKey{57373, 21, '~'},
    FunctionalKey{57374, 23, '~'}, FunctionalKey{57375, 24, '~'},
    FunctionalKey{57356, 7, '~'}, FunctionalKey{57357, 8, '~'},
    FunctionalKey{57364, 11, '~'}, FunctionalKey{57365, 12, '~'},
    FunctionalKey{57367, 14, '~'}, FunctionalKey{57363, 29, '~'}
};

[[nodiscard]] inline auto decode_modifiers(std::string_view field, Key& key) -> bool {
    const auto has_event = field.find(':') != std::string_view::npos;
    unsigned encoded{};
    if (!number(take(field, ':'), encoded) || encoded == 0) return false;
    key.modifiers = encoded - 1;
    return !has_event ||
           (number(field, key.event_type) && key.event_type >= 1 && key.event_type <= 3);
}

[[nodiscard]] inline auto decode_key(std::string_view parameters) -> Result<Event> {
    const Error invalid{2, "invalid CSI u sequence"};
    auto keys = take(parameters, ';');
    auto modifier_field = take(parameters, ';');
    Key key;
    if (keys.ends_with(':')) return invalid;
    if (!codepoint(take(keys, ':'), key.codepoint)) return invalid;
    const bool has_base_layout = keys.find(':') != std::string_view::npos;
    const auto shifted = take(keys, ':');
    char32_t code{};
    if (!shifted.empty()) {
        if (!codepoint(shifted, code)) return invalid;
        key.shifted = code;
    }
    if (has_base_layout) {
        if (!codepoint(keys, code)) return invalid;
        key.base_layout = code;
    }
    if (!modifier_field.empty() && !decode_modifiers(modifier_field, key)) return invalid;
    if (!parameters.empty()) {
        if (parameters.back() == ':') return invalid;
        do {
            if (!codepoint(take(parameters, ':'), code) || !text_codepoint(code)) return invalid;
            key.text.push_back(code);
        } while (!parameters.empty());
    }
    return Event{std::move(key)};
}

[[nodiscard]] inline auto decode_functional(std::string_view parameters,
                                             const char final) -> Result<Event> {
    if (parameters.empty() && final == 'Z') return Event{Key{U'\t', 1}};
    unsigned number_field = 1;
    Key key;
    if (!parameters.empty()) {
        const bool has_modifiers = parameters.find(';') != std::string_view::npos;
        if (!number(take(parameters, ';'), number_field) ||
            (has_modifiers && !decode_modifiers(parameters, key))) {
            return Error{2, "invalid functional key sequence"};
        }
    } else if (final == '~') {
        return Error{2, "missing functional key number"};
    }
    for (const auto& entry : functional_keys) {
        if (entry.parameter == number_field && entry.final == final) {
            key.codepoint = entry.code;
            return Event{std::move(key)};
        }
    }
    return Error{1, "unsupported input sequence"};
}

}  // namespace detail

/** @brief Decode a complete keyboard, SGR (1006), or bracketed-paste report.
 *  A single printable UTF-8 scalar is also accepted as a legacy key.
 */
[[nodiscard]] inline auto decode_seq(const std::string_view sequence) -> Result<Event> {
    constexpr std::string_view csi{"\x1B["};
    if (sequence.size() == 1) {
        switch (sequence.front()) {
        case '\t': case '\r': case '\x1B': case '\x7F':
            return Event{Key{static_cast<char32_t>(sequence.front())}};
        case '\b': return Event{Key{U'\x7F'}};
        default: break;
        }
    }
    const auto scalar = ttytk::detail::decode_utf8_scalar(sequence, 0);
    if (scalar.valid && scalar.length == sequence.size() && detail::text_codepoint(scalar.value)) {
        return Event{Key{scalar.value}};
    }
    if (sequence.size() == 3 && sequence.starts_with("\x1BO")) {
        if (sequence.back() == 'R') return Event{Key{57366}};
        return detail::decode_functional({}, sequence.back());
    }
    if (sequence.starts_with(detail::paste_start)) {
        const auto payload = sequence.substr(detail::paste_start.size());
        const auto end = payload.find(detail::paste_end);
        if (end == std::string_view::npos || end != payload.size() - detail::paste_end.size()) {
            return Error{1, "invalid bracketed paste"};
        }
        return Event{Paste{std::string{payload.substr(0, end)}}};
    }
    const auto prefix = sequence.starts_with(csi) ? csi.size() :
                        sequence.starts_with('\x9B') ? 1U : 0U;
    if (prefix == 0 || sequence.size() <= prefix) {
        return Error{1, "unsupported input sequence"};
    }
    const auto body = sequence.substr(prefix);
    const auto final = body.back();
    auto parameters = body.substr(0, body.size() - 1);
    if (final == 'u') return detail::decode_key(parameters);
    if ((final == 'M' || final == 'm') && parameters.starts_with('<')) {
        parameters.remove_prefix(1);
        unsigned encoded{}, x{}, y{};
        if (!detail::number(detail::take(parameters, ';'), encoded) || encoded >= 192 ||
            !detail::number(detail::take(parameters, ';'), x) || x == 0 ||
            !detail::number(parameters, y) || y == 0) {
            return Error{3, "invalid SGR mouse sequence"};
        }
        const auto button = encoded & detail::mouse_button_mask;
        const bool motion = (encoded & 32U) != 0;
        if ((button == 3 && !motion) || (motion && final != 'M')) {
            return Error{3, "invalid SGR mouse button"};
        }
        return Event{Mouse{button, encoded & detail::mouse_modifier_mask, x, y,
                           final == 'M' && button != 3, motion}};
    }
    return detail::decode_functional(parameters, final);
}

/** @brief Encode a key report using Kitty's CSI-u or legacy functional form. */
[[nodiscard]] inline auto encode_seq(const Key& key) -> Result<std::string> {
    if (!detail::scalar(key.codepoint) || (key.shifted && !detail::scalar(*key.shifted)) ||
        (key.base_layout && !detail::scalar(*key.base_layout)) ||
        key.modifiers == std::numeric_limits<unsigned>::max() || key.event_type < 1 ||
        key.event_type > 3) {
        return Error{2, "invalid CSI u key"};
    }
    if (!key.shifted && !key.base_layout && key.text.empty()) {
        for (const auto& entry : detail::functional_keys) {
            if (entry.code != key.codepoint) continue;
            std::string result{"\x1B["};
            if (key.modifiers == 0 && key.event_type == 1 &&
                (entry.final == 'P' || entry.final == 'Q' || entry.final == 'S')) {
                return std::string{"\x1BO"} + entry.final;
            }
            if (entry.final == '~' || key.modifiers != 0 || key.event_type != 1) {
                result += std::to_string(entry.parameter);
                if (key.modifiers != 0 || key.event_type != 1) {
                    result += ';' + std::to_string(key.modifiers + 1);
                    if (key.event_type != 1) result += ':' + std::to_string(key.event_type);
                }
            }
            return result + entry.final;
        }
    }
    std::string result = "\x1B[" + std::to_string(static_cast<unsigned>(key.codepoint));
    if (key.shifted || key.base_layout) {
        result += ':';
        if (key.shifted) result += std::to_string(static_cast<unsigned>(*key.shifted));
        if (key.base_layout) result += ':' + std::to_string(static_cast<unsigned>(*key.base_layout));
    }
    if (key.modifiers != 0 || key.event_type != 1 || !key.text.empty()) {
        result += ';' + std::to_string(key.modifiers + 1);
        if (key.event_type != 1) result += ':' + std::to_string(key.event_type);
    }
    if (!key.text.empty()) {
        result += ';';
        bool first = true;
        for (const auto code : key.text) {
            if (!detail::text_codepoint(code)) return Error{2, "invalid CSI u text"};
            if (!first) result += ':';
            result += std::to_string(static_cast<unsigned>(code));
            first = false;
        }
    }
    result += 'u';
    return result;
}

/** @brief Encode a mouse report in xterm's SGR (1006) format. */
[[nodiscard]] inline auto encode_seq(const Mouse& mouse) -> Result<std::string> {
    if ((mouse.button & ~detail::mouse_button_mask) != 0 || mouse.button >= 192 ||
        (mouse.modifiers & ~detail::mouse_modifier_mask) != 0 || mouse.x == 0 || mouse.y == 0 ||
        (mouse.motion ? mouse.pressed != (mouse.button != 3) : mouse.button == 3)) {
        return Error{3, "invalid SGR mouse event"};
    }
    const auto encoded = mouse.button | mouse.modifiers | (mouse.motion ? 32U : 0U);
    return "\x1B[<" + std::to_string(encoded) + ';' + std::to_string(mouse.x) +
           ';' + std::to_string(mouse.y) + (mouse.pressed || mouse.motion ? 'M' : 'm');
}

/** @brief Wrap pasted text in xterm's bracketed-paste delimiters. */
[[nodiscard]] inline auto encode_seq(const Paste& paste) -> Result<std::string> {
    if (paste.text.find(detail::paste_end) != std::string::npos) {
        return Error{1, "paste contains its terminator"};
    }
    return std::string{detail::paste_start} + paste.text + std::string{detail::paste_end};
}

}  // namespace ttytk::input
