#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-Automaton.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>

namespace ttytk {

/** Test driver for replaying terminal input and watching an observed output stream. */
class Puppet {
public:
    Puppet() {
        parser_.on_print = [this](const std::string_view text) { observed_ += text; };
        parser_.on_control = [this](const unsigned char control) {
            if (control == '\n' || control == '\t') observed_ += static_cast<char>(control);
        };
    }

    /** Append a valid Kitty CSI-u event to the synthetic input stream. */
    inline void send_key(const input::Key key) {
        const auto sequence = input::encode_seq(key);
        if (sequence) sent_ += sequence.value();
    }

    /** Append a valid xterm SGR mouse event; coordinates are one-based. */
    inline void send_mouse(const input::Mouse mouse) {
        const auto sequence = input::encode_seq(mouse);
        if (sequence) sent_ += sequence.value();
    }

    /** Wait for contiguous printed text; a zero timeout performs a single check. */
    [[nodiscard]] inline auto wait_for_text(const std::string_view text,
                                            const std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
        const -> bool {
        std::unique_lock lock{mutex_};
        const auto found = [&] { return observed_.find(text) != std::string::npos; };
        return timeout.count() <= 0 ? found() : changed_.wait_for(lock, timeout, found);
    }

    /** Feed output from the transport; may run on a separate thread from wait_for_text(). */
    inline void observe(const std::string_view bytes) {
        {
            const std::lock_guard lock{mutex_};
            parser_.feed(bytes);
        }
        changed_.notify_all();
    }

    [[nodiscard]] inline auto sent() const noexcept -> std::string_view { return sent_; }

private:
    mutable std::mutex mutex_{};
    mutable std::condition_variable changed_{};
    Automaton parser_{};
    std::string sent_{};
    std::string observed_{};
};

}  // namespace ttytk
