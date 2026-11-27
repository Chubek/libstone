#pragma once

#include <TTyTk/TTyTk-ClipboardTools.hpp>
#include <TTyUtils/TTyUtils-Config.hpp>
#include <TTyUtils/TTyUtils-Shell.hpp>

#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ttyutils::clip {

enum class Backend { Osc52, External, Internal };

struct Policy {
    bool allow_remote_read{false};
    std::size_t max_bytes{1024 * 1024};  ///< Maximum decoded payload for OSC 52.
    std::size_t max_history_bytes{1024 * 1024};  ///< Maximum size of one history entry.
    [[nodiscard]] static auto global() -> Policy& {
        static Policy policy;
        return policy;
    }
};

/** Application-owned terminal transport and external command I/O.
 *  No terminal queries or child processes are started by default. The
 *  request callback should wait only for the matching reply, with a timeout.
 *  External callbacks receive argument vectors, never shell source.
 */
struct Transport {
    std::function<bool(std::string_view)> send_osc{};
    std::function<std::optional<std::string>(std::string_view)> query_osc{};
    std::function<bool(const std::vector<std::string>&, std::string_view)> write_command{};
    std::function<std::optional<std::string>(const std::vector<std::string>&)> read_command{};
};

class History {
public:
    [[nodiscard]] static auto global() -> History& {
        static History history;
        return history;
    }
    void push(std::string value) {
        if (value.size() > Policy::global().max_history_bytes) return;
        entries_.push_front(std::move(value));
        if (entries_.size() > 32) entries_.pop_back();
    }
    void clear() noexcept { entries_.clear(); }
    [[nodiscard]] auto entry(const std::size_t index) const -> std::optional<std::string> {
        return index < entries_.size() ? std::optional{entries_[index]} : std::nullopt;
    }

private:
    std::deque<std::string> entries_;
};

/** Try the terminal, then a native helper, and always retain eligible history. */
inline auto copy(const std::string_view text, const Transport& transport, const Policy& policy = Policy::global())
    -> bool {
    bool copied = false;
    if (transport.send_osc && text.size() <= policy.max_bytes)
        copied = transport.send_osc(ttytk::clipboard::set_osc52(text));
    if (!copied && transport.write_command) {
        for (const auto& argv : std::array<std::vector<std::string>, 3>{
                 std::vector<std::string>{"wl-copy"}, {"xclip", "-selection", "clipboard"}, {"pbcopy"}}) {
            if ((copied = transport.write_command(argv, text))) break;
        }
    }
    if (text.size() <= policy.max_history_bytes && text.size() <= Policy::global().max_history_bytes) {
        History::global().push(std::string(text));
        copied = true;
    }
    return copied;
}

inline auto copy(const std::string_view text) -> bool { return copy(text, Transport{}); }

/** Remote requests are denied before accessing any backend, including history. */
inline auto paste(const Transport& transport, const bool remote = false,
                  const Policy& policy = Policy::global()) -> std::optional<std::string> {
    if (remote && !policy.allow_remote_read) return std::nullopt;
    if (transport.query_osc) {
        if (const auto reply = transport.query_osc(ttytk::clipboard::request_osc52())) {
            // At most four envelope bytes beyond canonical OSC/ST framing,
            // plus four base64 characters for each three decoded bytes.
            const auto size = reply->size();
            if (size <= 16 || (size - 16) / 4 <= policy.max_bytes / 3 + 1) {
                auto text = ttytk::clipboard::parse_osc52(*reply);
                if (text && text->size() <= policy.max_bytes) return text;
            }
        }
    }
    if (transport.read_command) {
        for (const auto& argv : std::array<std::vector<std::string>, 3>{
                 std::vector<std::string>{"wl-paste", "--no-newline"}, {"xclip", "-o", "-selection", "clipboard"},
                 {"pbpaste"}}) {
            if (auto text = transport.read_command(argv)) return text;
        }
    }
    return History::global().entry(0);
}

inline auto paste() -> std::optional<std::string> { return paste(Transport{}); }
inline auto paste(const bool remote) -> std::optional<std::string> {
    return paste(Transport{}, remote);
}

}  // namespace ttyutils::clip
