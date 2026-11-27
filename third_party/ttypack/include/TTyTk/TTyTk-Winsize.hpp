#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-SignalTools.hpp>

#include <cerrno>
#include <cstring>
#include <functional>
#include <limits>
#include <sys/ioctl.h>
#include <utility>

namespace ttytk::winsize {

/** Terminal dimensions in cells and, when available, pixels. */
struct Size {
    unsigned rows{};
    unsigned columns{};
    unsigned pixel_width{};
    unsigned pixel_height{};
};

/** Query a terminal descriptor's current dimensions. */
[[nodiscard]] inline auto get(const int fd) -> Result<Size> {
    signals::dispatch_pending();
    ::winsize native_size{};
    if (::ioctl(fd, TIOCGWINSZ, &native_size) != 0) {
        return Error{errno, std::strerror(errno)};
    }
    return Size{native_size.ws_row, native_size.ws_col, native_size.ws_xpixel,
                native_size.ws_ypixel};
}

/** Set a terminal's dimensions, rejecting values that cannot fit the ioctl. */
[[nodiscard]] inline auto set(const int fd, const Size size) -> Result<void> {
    signals::dispatch_pending();
    ::winsize native_size{};
    if (size.rows > std::numeric_limits<decltype(native_size.ws_row)>::max() ||
        size.columns > std::numeric_limits<decltype(native_size.ws_col)>::max() ||
        size.pixel_width > std::numeric_limits<decltype(native_size.ws_xpixel)>::max() ||
        size.pixel_height > std::numeric_limits<decltype(native_size.ws_ypixel)>::max()) {
        return Error{EOVERFLOW, "terminal dimensions exceed ioctl field width"};
    }
    native_size.ws_row = static_cast<decltype(native_size.ws_row)>(size.rows);
    native_size.ws_col =
        static_cast<decltype(native_size.ws_col)>(size.columns);
    native_size.ws_xpixel =
        static_cast<decltype(native_size.ws_xpixel)>(size.pixel_width);
    native_size.ws_ypixel =
        static_cast<decltype(native_size.ws_ypixel)>(size.pixel_height);
    if (::ioctl(fd, TIOCSWINSZ, &native_size) != 0) {
        return Error{errno, std::strerror(errno)};
    }
    return {};
}

/**
 * Register a deferred resize callback, or remove it by passing an empty one.
 * It coexists with signals::on_sigwinch() and runs at signals::dispatch_pending()
 * or when get()/set() dispatch pending signals.
 */
inline auto on_change(std::function<void()> callback) -> Result<void> {
    return signals::detail::subscribe(SIGWINCH, true, std::move(callback));
}

}  // namespace ttytk::winsize
