#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <charconv>
#include <functional>
#include <string_view>
#include <utility>

namespace ttytk::caps {
struct Info {
    bool has_sixel{};
    bool has_kitty_graphics{};
    bool has_truecolor{};
    bool has_bracketed_paste{};
    std::vector<unsigned> primary_device_attributes{};
    std::vector<unsigned> secondary_device_attributes{};
    std::optional<unsigned> device_status{};
    std::optional<std::pair<unsigned, unsigned>> cursor_position{};  ///< One-based row, column.
    std::optional<std::string> sgr_status{};
};
namespace detail {
inline auto callback() -> std::function<void(const Info&)>& {
    thread_local std::function<void(const Info&)> value;
    return value;
}

inline auto parameters(std::string_view body) -> std::vector<unsigned> {
    std::vector<unsigned> values;
    while (!body.empty()) {
        const auto end = body.find(';');
        const auto token = body.substr(0, end);
        unsigned number = 0;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) return {};
        values.push_back(number);
        if (end == std::string_view::npos) break;
        body.remove_prefix(end + 1);
        if (body.empty()) return {};
    }
    return values;
}

inline void csi(Info& info, const std::string_view body) {
    if (body.starts_with('?') && body.ends_with('c')) {
        const auto values = parameters(body.substr(1, body.size() - 2));
        if (values.empty()) return;
        info.primary_device_attributes = values;
        for (std::size_t i = 1; i < values.size(); ++i)
            if (values[i] == 4) info.has_sixel = true;
    } else if (body.starts_with('>') && body.ends_with('c')) {
        auto values = parameters(body.substr(1, body.size() - 2));
        if (values.size() == 3) info.secondary_device_attributes = std::move(values);
    } else if (body.ends_with('n')) {
        const auto values = parameters(body.substr(0, body.size() - 1));
        if (values.size() == 1) info.device_status = values.front();
    } else if (body.ends_with('R')) {
        const auto values = parameters(body.substr(0, body.size() - 1));
        if (values.size() == 2 && values[0] != 0 && values[1] != 0)
            info.cursor_position = {values[0], values[1]};
    } else if (body.starts_with('?') && body.ends_with("$y")) {
        const auto values = parameters(body.substr(1, body.size() - 3));
        if (values.size() == 2 && values[0] == 2004 && values[1] > 0 && values[1] <= 4)
            info.has_bracketed_paste = true;
    }
}

inline void dcs(Info& info, std::string_view body) {
    if (body.starts_with("1$r") && body.ends_with('m')) {
        info.sgr_status = std::string(body.substr(3));
    } else if (body.starts_with("1+r")) {
        body.remove_prefix(3);
        while (!body.empty()) {
            const auto end = body.find(';');
            const auto item = body.substr(0, end);
            const auto name = item.substr(0, item.find('='));
            if (name == "524742" || name == "5463") info.has_truecolor = true;  // RGB or Tc
            if (end == std::string_view::npos) break;
            body.remove_prefix(end + 1);
        }
    }
}
}  // namespace detail

/** Builds the DA, DA2, DSR, CPR, DECRQSS, DECRQM, XTGETTCAP and Kitty probes.
 *  The caller owns transport, query policy, timeout and response collection.
 */
[[nodiscard]] inline auto request() -> std::string_view {
    return "\x1b[c\x1b[>c\x1b[5n\x1b[6n\x1bP$qm\x1b\\"
           "\x1b[?2004$p\x1bP+q524742;5463\x1b\\"
           "\x1b_Gi=31,a=q,t=d,f=24,s=1,v=1;AAAA\x1b\\";
}

/** Parses a batch of complete, framed terminal capability responses. */
[[nodiscard]] inline auto query(const std::string_view response) -> Info {
    Info info;
    Automaton parser;
    parser.on_csi = [&info](const std::string_view body) { detail::csi(info, body); };
    parser.on_dcs = [&info](const std::string_view body) { detail::dcs(info, body); };
    parser.feed(response);
    // APC is not dispatched by Automaton; only accept the matching query ID.
    info.has_kitty_graphics = response.find("\x1b_Gi=31;OK\x1b\\") != std::string_view::npos;
    return info;
}

/** Registers a one-shot callback for on_response on the current thread. */
inline void query_async(std::function<void(const Info&)> callback) { detail::callback() = std::move(callback); }
inline void on_response(const std::string_view response) {
    auto callback = std::exchange(detail::callback(), {});
    if (callback) callback(query(response));
}
}  // namespace ttytk::caps
