#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-InputProto.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ttytk {

/** @brief Dispatch decoded keys, literal input and named key sequences. */
class KeyListener {
public:
    std::function<void(const input::Key&)> on_key{};
    std::function<void(std::string_view)> on_raw{};

    /**
     * @brief Bind a chord or a sequence of chords separated by single spaces.
     *
     * A binding that is also a sequence prefix waits for the next key or
     * flush(). An empty callback removes the binding.
     */
    inline void bind(std::string key, std::function<void()> callback) {
        if (key.empty()) return;
        if (callback) {
            bindings_[std::move(key)] = std::make_shared<std::function<void()>>(std::move(callback));
        } else {
            bindings_.erase(key);
        }
    }

    /** @brief Deliver raw input that has not been decoded as a key. */
    inline void dispatch_raw(const std::string_view bytes) {
        if (on_raw) on_raw(bytes);
    }

    /**
     * @brief Observe a key and match press/repeat events against bindings.
     *
     * Supply a chord spelling to override the default name. Raw bytes, if
     * available, are delivered separately from the spelling.
     */
    inline void dispatch(const input::Key& key, const std::string_view spelling = {},
                         const std::string_view raw = {}) {
        // Take ownership before callbacks, which may invalidate caller views.
        const auto chord = spelling.empty() ? key_name(key) : std::string{spelling};
        if (!raw.empty()) dispatch_raw(raw);
        if (on_key) on_key(key);
        if (key.event_type != 1 && key.event_type != 2) return;
        if (chord.empty()) {
            flush();
            return;
        }
        if (!pending_.empty()) {
            const auto combined = pending_ + ' ' + chord;
            if (match(combined)) return;
            flush();
        }
        match(chord);
    }

    /** @brief Resolve a pending exact binding, e.g. when a shortcut timer expires. */
    inline void flush() {
        pending_.clear();
        const auto found = bindings_.find(std::exchange(deferred_, {}));
        if (found != bindings_.end()) {
            auto callback = found->second;
            (*callback)();
        }
    }

    /** @brief Forget an incomplete binding without firing it. */
    inline void reset() noexcept {
        pending_.clear();
        deferred_.clear();
    }

private:
    static inline auto key_name(const input::Key& key) -> std::string {
        std::string name;
        if ((key.modifiers & 4U) != 0) name += "Ctrl-";
        if ((key.modifiers & 2U) != 0) name += "Alt-";
        if ((key.modifiers & 1U) != 0) name += "Shift-";
        if ((key.modifiers & 8U) != 0) name += "Super-";
        if ((key.modifiers & 16U) != 0) name += "Hyper-";
        if ((key.modifiers & 32U) != 0) name += "Meta-";
        auto code = key.base_layout.value_or(key.codepoint);
        switch (code) {
        case 9: return name + "Tab";
        case 13: return name + "Enter";
        case 27: return name + "Escape";
        case 32: return name + "Space";
        case 8: case 127: return name + "Backspace";
        case 57348: return name + "Insert";
        case 57349: return name + "Delete";
        case 57350: return name + "Left";
        case 57351: return name + "Right";
        case 57352: return name + "Up";
        case 57353: return name + "Down";
        case 57354: return name + "PageUp";
        case 57355: return name + "PageDown";
        case 57356: return name + "Home";
        case 57357: return name + "End";
        case 57363: return name + "Menu";
        default: break;
        }
        if (code >= 57364 && code <= 57398) {
            return name + 'F' + std::to_string(static_cast<unsigned>(code - 57363));
        }
        if (code >= U'a' && code <= U'z') {
            // Bind unmodified text as lowercase, and chords like Ctrl-C.
            if (!name.empty()) code -= U'a' - U'A';
        }
        if (code >= U'!' && code <= U'~') return name + static_cast<char>(code);
        if (code > 0) return name + "Key(" + std::to_string(static_cast<unsigned>(code)) + ')';
        return {};
    }

    inline auto match(const std::string& chord) -> bool {
        const auto prefix = chord + ' ';
        const auto found = bindings_.find(chord);
        for (const auto& binding : bindings_) {
            if (binding.first.starts_with(prefix)) {
                pending_ = chord;
                if (found != bindings_.end()) deferred_ = chord;
                return true;
            }
        }
        if (found == bindings_.end()) return false;
        reset();
        auto callback = found->second;
        (*callback)();
        return true;
    }

    std::unordered_map<std::string, std::shared_ptr<std::function<void()>>> bindings_{};
    std::string pending_{};
    std::string deferred_{};
};

}  // namespace ttytk
