#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <cstddef>
#include <limits>

namespace ttytk::flow {

/** Bound the number of bytes sent but not yet acknowledged. */
class Throttle {
public:
    explicit Throttle(const std::size_t capacity = 65536) : capacity_(capacity) {}

    /** Try to reserve capacity without blocking; false means retry later. */
    [[nodiscard]] inline auto wait_for_capacity(const std::size_t bytes) noexcept
        -> bool {
        if (bytes > capacity_ - used_) return false;
        used_ += bytes;
        return true;
    }

    inline void acknowledge(const std::size_t bytes) noexcept {
        used_ = bytes >= used_ ? 0 : used_ - bytes;
    }

private:
    std::size_t capacity_{};
    std::size_t used_{};
};

/** Track queued bytes and report when they reach the high watermark. */
class Backpressure {
public:
    explicit Backpressure(const std::size_t high_watermark = 65536)
        : high_watermark_(high_watermark) {}

    inline void add(const std::size_t bytes) noexcept {
        pending_ = bytes > std::numeric_limits<std::size_t>::max() - pending_
                       ? std::numeric_limits<std::size_t>::max()
                       : pending_ + bytes;
    }

    inline void consume(const std::size_t bytes) noexcept {
        pending_ = bytes >= pending_ ? 0 : pending_ - bytes;
    }

    [[nodiscard]] inline auto active() const noexcept -> bool {
        return pending_ >= high_watermark_;
    }

private:
    std::size_t high_watermark_{};
    std::size_t pending_{};
};

/** Track the transmission state controlled by VT100 DC1 and DC3. */
class XonXoff {
public:
    inline void feed(const unsigned char byte) noexcept {
        if (byte == 0x13U) paused_ = true;  // DC3 / XOFF
        if (byte == 0x11U) paused_ = false; // DC1 / XON
    }

    [[nodiscard]] inline auto paused() const noexcept -> bool {
        return paused_;
    }

private:
    bool paused_{};
};

}  // namespace ttytk::flow
