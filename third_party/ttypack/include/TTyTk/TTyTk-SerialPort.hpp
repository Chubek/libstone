#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

#if defined(TTYTK_PLATFORM_LINUX)
#include <cstdio>
#include <linux/serial.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#endif

namespace ttytk {

/** A nonblocking POSIX serial device with explicit line configuration. */
class Serial {
public:
    /** Select how the driver controls transmission. */
    enum class Handshake { None, XonXoff, RtsCts };

    Serial() = default;
    Serial(const Serial&) = delete;
    auto operator=(const Serial&) -> Serial& = delete;

    Serial(Serial&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    auto operator=(Serial&& other) noexcept -> Serial& {
        if (this != &other) {
            close_port();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    inline ~Serial() {
        close_port();
    }

    /** Open a serial device at the requested baud, initially raw 8N1. */
    [[nodiscard]] static inline auto open(const std::string& path,
                                          const unsigned baud)
        -> Result<Serial> {
        if (path.empty() || path.find('\0') != std::string::npos) {
            return Error{EINVAL, "invalid serial device path"};
        }
        const int descriptor = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (descriptor < 0) return Error{errno, std::strerror(errno)};
        Serial serial{descriptor};
        auto baud_result = serial.set_baud(baud);
        if (!baud_result) return baud_result.error();
        auto params = serial.set_params();
        if (!params) return params.error();
        auto handshake = serial.set_handshake(Handshake::None);
        if (!handshake) return handshake.error();
        return serial;
    }

    /** Borrow the serial descriptor; it remains owned by this object. */
    [[nodiscard]] inline auto native_fd() const noexcept -> int { return fd_; }

    /** Set input and output baud rates. */
    [[nodiscard]] inline auto set_baud(const unsigned baud) -> Result<void> {
        if (fd_ < 0) return Error{EBADF, "serial port is not open"};
        const auto speed = baud_constant(baud);
        if (!speed) return Error{EINVAL, "unsupported baud rate"};
        termios settings{};
        if (::tcgetattr(fd_, &settings) != 0) {
            return Error{errno, std::strerror(errno)};
        }
        if (::cfsetispeed(&settings, *speed) != 0 ||
            ::cfsetospeed(&settings, *speed) != 0 ||
            ::tcsetattr(fd_, TCSANOW, &settings) != 0) {
            return Error{errno, std::strerror(errno)};
        }
        return {};
    }

    /** Set raw-mode line format without changing the handshake policy. */
    [[nodiscard]] inline auto set_params(const unsigned data_bits = 8,
                                         const char parity = 'N',
                                         const unsigned stop_bits = 1)
        -> Result<void> {
        if (fd_ < 0) return Error{EBADF, "serial port is not open"};
        termios settings{};
        if (::tcgetattr(fd_, &settings) != 0) {
            return Error{errno, std::strerror(errno)};
        }
        constexpr auto software_flow = IXON | IXOFF | IXANY;
        const auto preserved_flow = settings.c_iflag & software_flow;
        ::cfmakeraw(&settings);
        settings.c_iflag = (settings.c_iflag & ~software_flow) | preserved_flow;
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= ~(CSIZE | PARENB | PARODD);
#if defined(CMSPAR)
        settings.c_cflag &= ~CMSPAR;
#endif
        switch (data_bits) {
        case 5: settings.c_cflag |= CS5; break;
        case 6: settings.c_cflag |= CS6; break;
        case 7: settings.c_cflag |= CS7; break;
        case 8: settings.c_cflag |= CS8; break;
        default: return Error{EINVAL, "data bits must be 5 through 8"};
        }
        if (parity == 'N') {
            settings.c_iflag &= ~(INPCK | IGNPAR | PARMRK);
        } else if (parity == 'E') {
            settings.c_cflag |= PARENB;
            settings.c_iflag |= INPCK;
        } else if (parity == 'O') {
            settings.c_cflag |= PARENB | PARODD;
            settings.c_iflag |= INPCK;
        } else {
            return Error{EINVAL, "parity must be N, E, or O"};
        }
        if (stop_bits == 1) settings.c_cflag &= ~CSTOPB;
        else if (stop_bits == 2) settings.c_cflag |= CSTOPB;
        else return Error{EINVAL, "stop bits must be 1 or 2"};
        if (::tcsetattr(fd_, TCSANOW, &settings) != 0) {
            return Error{errno, std::strerror(errno)};
        }
        return {};
    }

    /** Select no, XON/XOFF, or RTS/CTS handshake. */
    [[nodiscard]] inline auto set_handshake(const Handshake handshake) -> Result<void> {
        if (fd_ < 0) return Error{EBADF, "serial port is not open"};
        termios settings{};
        if (::tcgetattr(fd_, &settings) < 0) return Error{errno, std::strerror(errno)};
        settings.c_iflag &= ~(IXON | IXOFF | IXANY);
#if defined(CRTSCTS)
        constexpr auto hardware_flow = CRTSCTS;
#elif defined(CCTS_OFLOW) && defined(CRTS_IFLOW)
        constexpr auto hardware_flow = CCTS_OFLOW | CRTS_IFLOW;
#else
        constexpr tcflag_t hardware_flow = 0;
#endif
        settings.c_cflag &= ~hardware_flow;
        switch (handshake) {
        case Handshake::None: break;
        case Handshake::XonXoff:
            settings.c_iflag |= IXON | IXOFF;
            settings.c_cc[VSTART] = 0x11;  // DC1 / XON
            settings.c_cc[VSTOP] = 0x13;   // DC3 / XOFF
            break;
        case Handshake::RtsCts:
            if (hardware_flow == 0) return Error{ENOTSUP, "RTS/CTS is not supported"};
            settings.c_cflag |= hardware_flow;
            break;
        default: return Error{EINVAL, "invalid serial handshake"};
        }
        if (::tcsetattr(fd_, TCSANOW, &settings) < 0) return Error{errno, std::strerror(errno)};
        return {};
    }

    /** Report serial-device backing where the OS provides evidence of it. */
    [[nodiscard]] inline auto is_hardware() const noexcept -> bool {
        if (fd_ < 0) return false;
#if defined(TTYTK_PLATFORM_LINUX)
        ::serial_struct info{};
        if (::ioctl(fd_, TIOCGSERIAL, &info) == 0 && info.type != PORT_UNKNOWN) return true;
        struct stat metadata {};
        if (::fstat(fd_, &metadata) < 0 || !S_ISCHR(metadata.st_mode)) return false;
        // USB and other bus-backed tty drivers need not implement TIOCGSERIAL.
        char device[80]{};
        std::snprintf(device, sizeof(device), "/sys/dev/char/%u:%u/device",
                      major(metadata.st_rdev), minor(metadata.st_rdev));
        return ::stat(device, &metadata) == 0;
#else
        char device[256]{};
        if (::ttyname_r(fd_, device, sizeof(device)) != 0) return false;
        const std::string_view name{device};
        return name.starts_with("/dev/cu.") || name.starts_with("/dev/tty.");
#endif
    }

private:
    explicit Serial(const int fd) : fd_(fd) {}

    [[nodiscard]] static inline auto baud_constant(const unsigned baud)
        -> std::optional<speed_t> {
        switch (baud) {
        case 0: return B0;
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
#if defined(B230400)
        case 230400: return B230400;
#endif
#if defined(B460800)
        case 460800: return B460800;
#endif
#if defined(B921600)
        case 921600: return B921600;
#endif
        default: return std::nullopt;
        }
    }

    inline void close_port() noexcept {
        if (fd_ >= 0) static_cast<void>(::close(fd_));
        fd_ = -1;
    }

    int fd_{-1};
};

}  // namespace ttytk
