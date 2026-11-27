#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace ttytk {

/** @brief Incremental DEC-style parser for terminal byte streams.
 *
 * Views supplied to callbacks are valid only until the callback returns.
 * Callbacks must not call feed() or reset() on the same parser.
 */
class Automaton {
public:
    using SequenceHandler = std::function<void(std::string_view)>;
    using PrintHandler = std::function<void(std::string_view)>;

    /** @brief CSI parameters, intermediates and final (without the introducer). */
    SequenceHandler on_csi{};
    /** @brief Complete OSC body (without the introducer or terminator). */
    SequenceHandler on_osc{};
    /** @brief Raw printable bytes, which may split UTF-8 across calls. */
    PrintHandler on_print{};
    /** @brief Complete DCS header and payload (without the introducer or terminator). */
    SequenceHandler on_dcs{};
    /** @brief An executed C0 or standalone C1 control byte. */
    std::function<void(unsigned char)> on_control{};
    /** @brief ESC intermediates and final (without ESC). */
    SequenceHandler on_escape{};

    /** @brief Maximum collected bytes per control sequence; larger sequences are ignored. */
    std::size_t max_sequence_bytes{1024 * 1024};

    /** @brief Discard an unfinished sequence without changing callbacks or limits. */
    inline void reset() noexcept {
        state_ = State::ground;
        sequence_.clear();
        printable_.clear();
        utf8_remaining_ = 0;
    }

    /** @brief Consume bytes, retaining any unfinished sequence across feeds. */
    inline void feed(const std::string_view bytes) {
        for (const unsigned char byte : bytes) feed_byte(byte);
        flush_printable();
    }

    inline void feed(const char* const bytes, const std::size_t size) {
        feed(std::string_view{bytes, size});
    }

private:
    enum class State {
        ground, escape, escape_intermediate, escape_ignore,
        csi_entry, csi_param, csi_intermediate, csi_ignore,
        osc, osc_ignore, dcs_entry, dcs_param, dcs_intermediate, dcs_data, string_ignore,
        osc_escape, dcs_escape, ignored_string_escape
    };

    inline void flush_printable() {
        if (printable_.empty()) return;
        auto text = std::exchange(printable_, {});
        if (on_print) on_print(text);
    }

    inline void enter(const State next) {
        sequence_.clear();
        state_ = next;
    }

    inline void dispatch(SequenceHandler& handler) {
        auto value = std::exchange(sequence_, {});
        state_ = State::ground;
        if (handler) handler(value);
    }

    inline void collect(const unsigned char byte, const State overflow) {
        if (sequence_.size() >= max_sequence_bytes) enter(overflow);
        else sequence_.push_back(static_cast<char>(byte));
    }

    inline void execute(const unsigned char byte) {
        flush_printable();
        if (on_control) on_control(byte);
    }

    // C1 byte values occurring as UTF-8 continuations aren't standalone controls.
    // Keep this distinction across read boundaries, without decoding printable text.
    inline auto utf8_byte(const unsigned char byte) -> bool {
        if (utf8_remaining_ != 0) {
            if (byte >= utf8_min_ && byte <= utf8_max_) {
                --utf8_remaining_;
                utf8_min_ = 0x80U;
                utf8_max_ = 0xBFU;
                return true;
            }
            utf8_remaining_ = 0;
        }
        if (byte < 0xC2U || byte > 0xF4U) return false;
        utf8_remaining_ = byte < 0xE0U ? 1 : byte < 0xF0U ? 2 : 3;
        utf8_min_ = byte == 0xE0U ? 0xA0U : byte == 0xF0U ? 0x90U : 0x80U;
        utf8_max_ = byte == 0xEDU ? 0x9FU : byte == 0xF4U ? 0x8FU : 0xBFU;
        return true;
    }

    inline void terminate_string() {
        if (state_ == State::osc || state_ == State::osc_escape) dispatch(on_osc);
        else if (state_ == State::dcs_data || state_ == State::dcs_escape) dispatch(on_dcs);
        else enter(State::ground);
    }

    inline void header_byte(const unsigned char byte, const bool dcs) {
        const auto ignore = dcs ? State::string_ignore : State::csi_ignore;
        if (byte < 0x20U) {
            if (!dcs) execute(byte);
        } else if (byte >= 0x40U && byte <= 0x7EU) {
            collect(byte, ignore);
            if (state_ != ignore) {
                if (dcs) state_ = State::dcs_data;
                else dispatch(on_csi);
            } else if (!dcs) {
                enter(State::ground);
            }
        } else if (byte >= 0x20U && byte <= 0x2FU) {
            state_ = dcs ? State::dcs_intermediate : State::csi_intermediate;
            collect(byte, ignore);
        } else if (byte >= 0x30U && byte <= 0x3FU) {
            const bool entry = state_ == State::dcs_entry || state_ == State::csi_entry;
            const bool intermediate =
                state_ == State::dcs_intermediate || state_ == State::csi_intermediate;
            // Modern CSI permits colon-separated subparameters (e.g. SGR and Kitty).
            if (intermediate || (!entry && byte >= 0x3CU) || (dcs && byte == ':')) {
                enter(ignore);
            } else {
                state_ = dcs ? State::dcs_param : State::csi_param;
                collect(byte, ignore);
            }
        }
    }

    inline void feed_byte(const unsigned char byte) {
        if (state_ == State::osc_escape || state_ == State::dcs_escape ||
            state_ == State::ignored_string_escape) {
            if (byte == '\\') {
                terminate_string();
                return;
            }
            // ESC not followed by '\' starts a new escape, abandoning the string.
            enter(State::escape);
        }

        if ((state_ == State::ground || state_ == State::osc || state_ == State::osc_ignore ||
             state_ == State::dcs_data || state_ == State::string_ignore) && utf8_byte(byte)) {
            if (state_ == State::ground) printable_.push_back(static_cast<char>(byte));
            else if (state_ == State::osc) collect(byte, State::osc_ignore);
            else if (state_ == State::dcs_data) collect(byte, State::string_ignore);
            return;
        }

        // DEC "anywhere" transitions take precedence over state-local actions.
        if (byte == 0x1BU) {
            flush_printable();
            if (state_ == State::osc) state_ = State::osc_escape;
            else if (state_ == State::dcs_data) state_ = State::dcs_escape;
            else if (state_ == State::string_ignore || state_ == State::osc_ignore) {
                state_ = State::ignored_string_escape;
            } else enter(State::escape);
            return;
        }
        if (byte == 0x18U || byte == 0x1AU) {
            enter(State::ground);
            execute(byte);
            return;
        }
        if (byte >= 0x80U && byte <= 0x9FU) {
            flush_printable();
            switch (byte) {
            case 0x90U: enter(State::dcs_entry); break;
            case 0x9BU: enter(State::csi_entry); break;
            case 0x9CU: terminate_string(); break;
            case 0x9DU: enter(State::osc); break;
            case 0x98U: case 0x9EU: case 0x9FU: enter(State::string_ignore); break;
            default: enter(State::ground); execute(byte); break;
            }
            return;
        }

        switch (state_) {
        case State::ground:
            if (byte < 0x20U) execute(byte);
            else if (byte != 0x7FU) printable_.push_back(static_cast<char>(byte));
            break;
        case State::escape:
        case State::escape_intermediate:
        case State::escape_ignore: {
            const auto ascii = static_cast<unsigned char>(byte & 0x7FU);
            if (byte < 0x20U) {
                execute(byte);
            } else if (ascii >= 0x20U && ascii <= 0x2FU) {
                if (state_ != State::escape_ignore) {
                    state_ = State::escape_intermediate;
                    collect(ascii, State::escape_ignore);
                }
            } else if (ascii >= 0x30U && ascii <= 0x7EU) {
                if (state_ == State::escape) {
                    switch (ascii) {
                    case '[': enter(State::csi_entry); return;
                    case ']': enter(State::osc); return;
                    case 'P': enter(State::dcs_entry); return;
                    case 'X': case '^': case '_': enter(State::string_ignore); return;
                    default: break;
                    }
                }
                if (state_ != State::escape_ignore) collect(ascii, State::escape_ignore);
                if (state_ == State::escape_ignore) enter(State::ground);
                else dispatch(on_escape);
            }
            break;
        }
        case State::csi_entry: case State::csi_param: case State::csi_intermediate:
            header_byte(static_cast<unsigned char>(byte & 0x7FU), false);
            break;
        case State::csi_ignore:
            if (byte < 0x20U) execute(byte);
            else if ((byte & 0x7FU) >= 0x40U && (byte & 0x7FU) <= 0x7EU) enter(State::ground);
            break;
        case State::dcs_entry: case State::dcs_param: case State::dcs_intermediate:
            header_byte(static_cast<unsigned char>(byte & 0x7FU), true);
            break;
        case State::osc:
            // xterm also accepts BEL as the OSC terminator.
            if (byte == 0x07U) terminate_string();
            else if (byte >= 0x20U && byte != 0x7FU) collect(byte, State::osc_ignore);
            break;
        case State::osc_ignore:
            if (byte == 0x07U) enter(State::ground);
            break;
        case State::dcs_data:
            if (byte != 0x7FU) collect(byte, State::string_ignore);
            break;
        case State::string_ignore:
        case State::osc_escape: case State::dcs_escape: case State::ignored_string_escape:
            break;
        }
    }

    State state_{State::ground};
    std::string sequence_{};
    std::string printable_{};
    unsigned utf8_remaining_{};
    unsigned char utf8_min_{};
    unsigned char utf8_max_{};
};

}  // namespace ttytk
