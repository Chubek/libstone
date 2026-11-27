#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>

#include <string>
#include <string_view>

namespace ttytk::clipboard {
enum Selection { Global, Primary };
namespace detail {
inline constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline auto target(const Selection selection) -> char { return selection == Primary ? 'p' : 'c'; }
inline auto prefix(const Selection selection) -> std::string {
    return std::string{esc::osc} + "52;" + target(selection) + ';';
}
}  // namespace detail

inline auto storage(const Selection selection = Global) -> std::string& {
    static std::string primary, global;
    return selection == Primary ? primary : global;
}
inline void set(const std::string_view text, const Selection selection = Global) { storage(selection) = text; }
[[nodiscard]] inline auto get(const Selection selection = Global) -> std::string { return storage(selection); }
inline void clear(const Selection selection = Global) { storage(selection).clear(); }

/** Builds an OSC 52 query; the caller controls whether to transmit it. */
[[nodiscard]] inline auto request_osc52(const Selection selection = Global) -> std::string {
    return detail::prefix(selection) + "?\x1b\\";
}

/** Builds an OSC 52 update; untrusted text is carried only as base64. */
[[nodiscard]] inline auto set_osc52(const std::string_view text, const Selection selection = Global)
    -> std::string {
    auto out = detail::prefix(selection);
    unsigned bits = 0, count = 0;
    for (const unsigned char byte : text) {
        bits = (bits << 8U) | byte;
        count += 8;
        while (count >= 6) {
            count -= 6;
            out += detail::alphabet[(bits >> count) & 63U];
        }
    }
    if (count != 0) out += detail::alphabet[(bits << (6U - count)) & 63U];
    if (count == 2) out += "==";
    if (count == 4) out += '=';
    return out + "\x1b\\";
}

/** Decodes a reply for the requested selection, without changing local storage.
 *  Accepts either an OSC body from Automaton or one complete framed sequence.
 */
[[nodiscard]] inline auto parse_osc52(std::string_view reply, const Selection selection = Global)
    -> std::optional<std::string> {
    if (reply.starts_with(esc::osc) || reply.starts_with('\x9d')) {
        reply.remove_prefix(reply.starts_with(esc::osc) ? 2 : 1);
        if (reply.ends_with("\x1b\\")) reply.remove_suffix(2);
        else if (reply.ends_with('\a') || reply.ends_with('\x9c')) reply.remove_suffix(1);
        else return std::nullopt;
    }
    if (!reply.starts_with("52;")) return std::nullopt;
    reply.remove_prefix(3);
    if (reply.size() < 2 || reply[0] != detail::target(selection) || reply[1] != ';')
        return std::nullopt;
    reply.remove_prefix(2);
    if (reply.size() % 4 != 0) return std::nullopt;
    auto padding = std::size_t{0};
    while (!reply.empty() && reply.back() == '=') {
        ++padding;
        reply.remove_suffix(1);
    }
    if (padding > 2) return std::nullopt;
    std::string out;
    unsigned bits = 0, count = 0;
    for (const char c : reply) {
        const auto digit = detail::alphabet.find(c);
        if (digit == std::string_view::npos) return std::nullopt;
        bits = (bits << 6U) | static_cast<unsigned>(digit);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 255U);
        }
    }
    if ((bits & ((1U << count) - 1)) != 0) return std::nullopt;
    return out;
}
}  // namespace ttytk::clipboard

// NOTE(agent): TTyUtils refers to TTyTk::Clipboard, while the manifest's
// owning header exposes the clipboard namespace functions. This façade keeps
// both spellings on the same storage.
namespace ttytk {
class Clipboard {
public:
    void set(const std::string_view text) { clipboard::set(text); }
    [[nodiscard]] auto get() const -> std::string { return clipboard::get(); }
    void clear() { clipboard::clear(); }
};
}  // namespace ttytk
