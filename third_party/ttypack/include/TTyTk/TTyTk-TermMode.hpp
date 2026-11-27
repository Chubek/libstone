#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-EscapeCodes.hpp>

#include <array>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace ttytk {

/** Common DEC private modes selected by CSI ? Ps h or CSI ? Ps l. */
enum class Mode : unsigned {
    CursorKeys = 1,
    Origin = 6,
    AutoWrap = 7,
    MouseX10 = 9,
    CursorVisible = 25,
    AltScreenLegacy = 47,
    Mouse = 1000,
    MouseHighlight = 1001,
    MouseButtonEvent = 1002,
    MouseAnyEvent = 1003,
    FocusReport = 1004,
    MouseUtf8 = 1005,
    MouseSgr = 1006,
    MouseUrxvt = 1015,
    MouseSgrPixels = 1016,
    AltScreenClear = 1047,
    AltScreen = 1049,
    BracketedPaste = 2004,
};

/** ANSI SM/RM mode selectors (no '?' prefix). */
enum class AnsiMode : unsigned {
    KeyboardAction = 2,
    Insert = 4,
    SendReceive = 12,
    NewLine = 20,
};

/** State for independently numbered ANSI and DEC modes. */
class TermMode {
public:
    inline void set(const Mode mode) { set(static_cast<unsigned>(mode), true); }
    inline void set(const AnsiMode mode) { set(static_cast<unsigned>(mode), false); }

    /** Set a numeric mode; private_mode describes the presence of '?'. */
    inline void set(const unsigned number, const bool private_mode) {
        current_.emplace(private_mode, number);
        if (!private_mode) return;
        exclusive(number, std::array{9U, 1000U, 1001U, 1002U, 1003U});
        exclusive(number, std::array{1005U, 1006U, 1015U, 1016U});
    }

    inline void reset(const Mode mode) { reset(static_cast<unsigned>(mode), true); }
    inline void reset(const AnsiMode mode) { reset(static_cast<unsigned>(mode), false); }

    /** Reset a single numeric mode in its namespace. */
    inline void reset(const unsigned number, const bool private_mode) {
        current_.erase({private_mode, number});
    }

    [[nodiscard]] inline auto test(const Mode mode) const noexcept -> bool {
        return test(static_cast<unsigned>(mode), true);
    }

    [[nodiscard]] inline auto test(const AnsiMode mode) const noexcept -> bool {
        return test(static_cast<unsigned>(mode), false);
    }

    /** Check a single numeric mode in its namespace. */
    [[nodiscard]] inline auto test(const unsigned number, const bool private_mode)
        const noexcept -> bool {
        return current_.contains({private_mode, number});
    }

    /** Snapshot all ANSI and DEC mode states. */
    inline void save() {
        saved_ = current_;
    }

    /** Restore the full snapshot, if one has been saved. */
    inline void restore() {
        if (saved_) current_ = *saved_;
    }

    /** Remember a DEC mode's state (CSI ? Ps s), including a reset mode. */
    inline void save(const Mode mode) { save(static_cast<unsigned>(mode), true); }
    inline void save(const unsigned number, const bool private_mode) {
        saved_modes_[{private_mode, number}] = test(number, private_mode);
    }

    /** Restore just the remembered mode state (CSI ? Ps r for DEC modes). */
    inline void restore(const Mode mode) { restore(static_cast<unsigned>(mode), true); }
    inline void restore(const unsigned number, const bool private_mode) {
        const auto saved = saved_modes_.find({private_mode, number});
        if (saved == saved_modes_.end()) return;
        if (saved->second) set(number, private_mode);
        else reset(number, private_mode);
    }

private:
    template <std::size_t N>
    inline void exclusive(const unsigned number, const std::array<unsigned, N> modes) {
        bool found = false;
        for (const auto mode : modes) found |= mode == number;
        if (!found) return;
        for (const auto mode : modes) {
            if (mode != number) reset(mode, true);
        }
    }

    using Key = std::pair<bool, unsigned>;
    std::set<Key> current_{{true, 7}, {true, 25}};
    std::optional<std::set<Key>> saved_{};
    std::map<Key, bool> saved_modes_{};
};

}  // namespace ttytk
