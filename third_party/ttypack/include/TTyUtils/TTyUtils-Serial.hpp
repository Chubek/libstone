#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyUtils/TTyUtils-EventLoop.hpp>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ttyutils::serial {

enum class Flow { None, Hardware, Software };

struct Settings {
    unsigned baud{115200};
    Flow flow{Flow::None};
    unsigned data_bits{8};
    char parity{'N'};
    unsigned stop_bits{1};
};

class Port {
public:
    Port() = default;
    explicit Port(const int fd) : fd_(fd) {}
    Port(const Port&) = delete;
    auto operator=(const Port&) -> Port& = delete;
    Port(Port&& other) noexcept : fd_(std::exchange(other.fd_, -1)), callback_(std::move(other.callback_)) {
        other.watch_.cancel();
    }
    ~Port() { watch_.cancel(); if (fd_ >= 0) ::close(fd_); }

    [[nodiscard]] static auto open(const std::string& path, const Settings& settings = {}) -> std::optional<Port> {
        if (path.find('\0') != std::string::npos) return std::nullopt;
        const auto fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return std::nullopt;
        Port port{fd};
        if (!port.configure(settings)) return std::nullopt;
        return port;
    }
    [[nodiscard]] auto configure(const Settings& settings) -> bool {
        const auto speed = baud_rate(settings.baud);
        if (!speed || (settings.parity != 'N' && settings.parity != 'E' && settings.parity != 'O') ||
            (settings.stop_bits != 1 && settings.stop_bits != 2)) return false;
        termios state{};
        if (::tcgetattr(fd_, &state) != 0) return false;
        ::cfmakeraw(&state);
        state.c_cflag |= CLOCAL | CREAD;
        state.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
        state.c_iflag &= ~(IXON | IXOFF | IXANY | INPCK | IGNPAR);
        switch (settings.data_bits) {
        case 5: state.c_cflag |= CS5; break;
        case 6: state.c_cflag |= CS6; break;
        case 7: state.c_cflag |= CS7; break;
        case 8: state.c_cflag |= CS8; break;
        default: return false;
        }
        if (settings.parity == 'E') state.c_cflag |= PARENB;
        if (settings.parity == 'O') state.c_cflag |= PARENB | PARODD;
        if (settings.parity != 'N') state.c_iflag |= INPCK;
        if (settings.stop_bits == 2) state.c_cflag |= CSTOPB;
#ifdef CRTSCTS
        state.c_cflag &= ~CRTSCTS;
        if (settings.flow == Flow::Hardware) state.c_cflag |= CRTSCTS;
#else
        if (settings.flow == Flow::Hardware) return false;
#endif
        if (settings.flow == Flow::Software) state.c_iflag |= IXON | IXOFF;
        return ::cfsetispeed(&state, *speed) == 0 && ::cfsetospeed(&state, *speed) == 0 &&
               ::tcsetattr(fd_, TCSANOW, &state) == 0;
    }
    [[nodiscard]] auto fd() const noexcept -> int { return fd_; }
    [[nodiscard]] auto write(const std::string_view text) const -> bool {
        const auto count = write_some(text);
        return count && *count == text.size();
    }
    /// Report partial writes or would-block to callers doing non-blocking I/O.
    [[nodiscard]] auto write_some(const std::string_view text) const -> std::optional<std::size_t> {
        ssize_t result;
        do { result = ::write(fd_, text.data(), text.size()); } while (result < 0 && errno == EINTR);
        if (result < 0) return std::nullopt;
        return static_cast<std::size_t>(result);
    }
    [[nodiscard]] auto send_break() const noexcept -> bool { return ::tcsendbreak(fd_, 0) == 0; }
    void on_data(std::function<void(std::string)> callback) { callback_ = std::move(callback); }
    /// Bind read delivery to the port's lifetime. Moving the port detaches it.
    void attach(loop::Loop& loop) {
        watch_.cancel();
        if (fd_ >= 0) watch_ = loop.watch(fd_, loop::Readable, [this](const loop::Event&) { pump(); });
    }
    void pump() {
        char buffer[4096];
        ssize_t count;
        do { count = ::read(fd_, buffer, sizeof(buffer)); } while (count < 0 && errno == EINTR);
        if (count > 0) {
            if (auto callback = callback_) callback({buffer, static_cast<std::size_t>(count)});
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) watch_.cancel();
    }

private:
    static auto baud_rate(const unsigned baud) noexcept -> std::optional<speed_t> {
        switch (baud) {
        case 50: return B50;
        case 75: return B75;
        case 110: return B110;
        case 134: return B134;
        case 150: return B150;
        case 200: return B200;
        case 300: return B300;
        case 600: return B600;
        case 1200: return B1200;
        case 1800: return B1800;
        case 2400: return B2400;
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
        default: return std::nullopt;
        }
    }
    int fd_{-1};
    std::function<void(std::string)> callback_{};
    loop::FdWatch watch_;
};

class Modem {
public:
    explicit Modem(Port& port) : port_(&port) {}
    [[nodiscard]] auto carrier_detected() const noexcept -> bool {
        int status = 0;
        return ::ioctl(port_->fd(), TIOCMGET, &status) == 0 && (status & TIOCM_CAR) != 0;
    }
    [[nodiscard]] auto set_dtr(const bool value) const noexcept -> bool { return set(TIOCM_DTR, value); }
    [[nodiscard]] auto set_rts(const bool value) const noexcept -> bool { return set(TIOCM_RTS, value); }

private:
    auto set(int line, const bool value) const noexcept -> bool {
        return ::ioctl(port_->fd(), value ? TIOCMBIS : TIOCMBIC, &line) == 0;
    }
    Port* port_;
};

inline auto enumerate() -> std::vector<std::string> {
    std::vector<std::string> result;
    std::error_code error;
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator at("/dev", error); !error && at != end; at.increment(error)) {
        const auto& entry = *at;
        const auto name = entry.path().filename().string();
        if (name.starts_with("ttyUSB") || name.starts_with("ttyACM") || name.starts_with("cu."))
            result.push_back(entry.path().string());
    }
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace ttyutils::serial
