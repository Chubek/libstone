#pragma once

#include <TTyTk/TTyTk-Encoding.hpp>
#include <TTyTk/TTyTk-NotifyTools.hpp>
#include <TTyUtils/TTyUtils-Shell.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace ttyutils::notify {

enum class Urgency { Low, Normal, Critical };
enum class Backend { Terminal, External, Bell };

struct Message {
    std::string title{};
    std::string body{};
    Urgency urgency{Urgency::Normal};

    friend auto operator==(const Message&, const Message&) -> bool = default;
};

struct Policy {
    bool do_not_disturb{};
    bool allow_external{true};
    std::optional<Backend> backend{};
    std::chrono::milliseconds deduplicate_for{1000};
};

/// Policy for calls on the notification/UI thread.
inline auto policy() noexcept -> Policy& {
    static Policy value;
    return value;
}

namespace detail {
inline auto output(const std::string_view bytes, FILE* const stream) -> bool {
    return std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size() && std::fflush(stream) == 0;
}
inline auto environment(const char* key) -> std::string_view {
    const auto* value = std::getenv(key);
    return value ? std::string_view(value) : std::string_view{};
}
inline auto safe_osc_field(const std::string_view value) -> std::string {
    auto text = ttytk::utf8::decode(value);
    for (auto& c : text) {
        if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == U';') c = U' ';
    }
    return ttytk::utf8::encode(text);
}
}  // namespace detail

/// Deliver a notification or fall back to BEL. Suppressed/undelivered messages return false.
inline auto send(const Message& message, FILE* const output = stdout) -> bool {
    using Clock = std::chrono::steady_clock;
    struct Recent { Message message; Clock::time_point sent; };
    static std::deque<Recent> recent;
    const auto config = policy();
    const auto now = Clock::now();
    if (!output || config.do_not_disturb || (message.title.empty() && message.body.empty())) return false;
    while (!recent.empty() && now - recent.front().sent >= config.deduplicate_for) recent.pop_front();
    if (std::any_of(recent.begin(), recent.end(), [&](const Recent& entry) { return entry.message == message; }))
        return false;

    const auto program = detail::environment("TERM_PROGRAM");
    const bool terminal = config.backend == Backend::Terminal ||
        (!config.backend && ::isatty(::fileno(output)) &&
         (program == "WezTerm" || program == "iTerm.app" || program == "mintty"));
    bool delivered = false;
    if (terminal) {
        const auto title = detail::safe_osc_field(message.title);
        const auto body = detail::safe_osc_field(message.body);
        delivered = detail::output(program == "iTerm.app" || program == "mintty"
            ? "\x1b]9;" + title + (body.empty() ? "" : ": " + body) + "\x1b\\"
            : ttytk::notify::show(title, body), output);
    }
    if (!delivered && config.allow_external && (config.backend == Backend::External ||
        (!config.backend && !detail::environment("DBUS_SESSION_BUS_ADDRESS").empty()))) {
        const auto urgency = message.urgency == Urgency::Critical ? "critical" :
                             message.urgency == Urgency::Low ? "low" : "normal";
        auto job = shell::run({"notify-send", "--urgency", urgency, "--",
                               message.title, message.body},
                              shell::Options{.capture = shell::Capture::Both,
                                             .timeout = std::chrono::milliseconds(500)});
        delivered = job.wait() == 0;
    }
    if (!delivered) delivered = detail::output("\a", output);
    if (delivered && config.deduplicate_for.count() > 0) {
        if (recent.size() >= 64) recent.pop_front();
        recent.push_back({message, Clock::now()});
    }
    return delivered;
}

inline void bell() {
    if (!policy().do_not_disturb) (void)detail::output("\a", stdout);
}

}  // namespace ttyutils::notify
