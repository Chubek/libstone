#pragma once

#include <TTyTk/TTyTk-Core.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace ttytk::ipc {

/** One length-prefixed Unix-domain stream message (binary payload allowed). */
struct Message {
    std::string payload{};
};

namespace detail {

inline auto address(const std::string& path) -> Result<sockaddr_un> {
    sockaddr_un value{};
    value.sun_family = AF_UNIX;
    if (path.empty() || path.find('\0') != std::string::npos) {
        return Error{EINVAL, "IPC socket path must be nonempty and contain no NUL"};
    }
    if (path.size() >= sizeof(value.sun_path)) {
        return Error{ENAMETOOLONG, "IPC socket path is too long"};
    }
    std::memcpy(value.sun_path, path.c_str(), path.size() + 1);
#if defined(TTYTK_PLATFORM_MACOS)
    value.sun_len = sizeof(value);
#endif
    return value;
}

inline auto configure_socket(const int fd) -> Result<void> {
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        return Error{errno, std::strerror(errno)};
    }
#if defined(TTYTK_PLATFORM_MACOS)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        return Error{errno, std::strerror(errno)};
    }
#endif
    return {};
}

inline auto open_socket() -> int {
#if defined(TTYTK_PLATFORM_LINUX)
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    return ::socket(AF_UNIX, SOCK_STREAM, 0);
#endif
}

}  // namespace detail

/** Own either a local listener or a connected, framed IPC stream. */
class Channel {
public:
    /** Maximum payload in bytes, checked before allocation. */
    static constexpr std::size_t max_payload_size = 16U * 1024U * 1024U;

    Channel() = default;
    Channel(const Channel&) = delete;
    auto operator=(const Channel&) -> Channel& = delete;

    Channel(Channel&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          listener_(std::exchange(other.listener_, false)),
          callback_(std::move(other.callback_)) {}

    auto operator=(Channel&& other) noexcept -> Channel& {
        if (this != &other) {
            close_channel();
            fd_ = std::exchange(other.fd_, -1);
            listener_ = std::exchange(other.listener_, false);
            callback_ = std::move(other.callback_);
        }
        return *this;
    }

    inline ~Channel() {
        close_channel();
    }

    /** Borrow the socket descriptor for polling. */
    [[nodiscard]] inline auto native_fd() const noexcept -> int { return fd_; }

    /**
     * Accept one queued peer and invoke the listener's callback on it.
     * Returns EAGAIN/EWOULDBLOCK when no peer is ready.
     */
    [[nodiscard]] inline auto accept() -> Result<Channel> {
        if (fd_ < 0) return Error{EBADF, "IPC channel is not open"};
        if (!listener_) return Error{EINVAL, "IPC channel is not a listener"};
        int peer;
        do {
#if defined(TTYTK_PLATFORM_LINUX)
            peer = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
#else
            peer = ::accept(fd_, nullptr, nullptr);
#endif
        } while (peer < 0 && errno == EINTR);
        if (peer < 0) return Error{errno, std::strerror(errno)};
        Channel connected{peer};
        auto configured = detail::configure_socket(peer);
        if (!configured) return configured.error();
        const int flags = ::fcntl(peer, F_GETFL);
        if (flags < 0 || ::fcntl(peer, F_SETFL, flags & ~O_NONBLOCK) < 0) {
            return Error{errno, std::strerror(errno)};
        }
        if (callback_) callback_(connected);
        return connected;
    }

    /** Send one complete message on a connected (blocking) stream. */
    [[nodiscard]] inline auto send(const Message& message) -> Result<void> {
        if (fd_ < 0) return Error{EBADF, "IPC channel is not open"};
        if (listener_) return Error{ENOTCONN, "IPC listener has no connected peer"};
        if (message.payload.size() > max_payload_size) {
            return Error{EMSGSIZE, "IPC message is too large"};
        }
        const auto size = static_cast<std::uint32_t>(message.payload.size());
        const std::array<unsigned char, 4> header{
            static_cast<unsigned char>((size >> 24U) & 0xFFU),
            static_cast<unsigned char>((size >> 16U) & 0xFFU),
            static_cast<unsigned char>((size >> 8U) & 0xFFU),
            static_cast<unsigned char>(size & 0xFFU),
        };
        auto header_result = write_all(header.data(), header.size());
        if (!header_result) return header_result;
        return write_all(
            reinterpret_cast<const unsigned char*>(message.payload.data()),
            message.payload.size());
    }

    /** Receive one complete message on a connected (blocking) stream. */
    [[nodiscard]] inline auto receive() -> Result<Message> {
        if (fd_ < 0) return Error{EBADF, "IPC channel is not open"};
        if (listener_) return Error{ENOTCONN, "IPC listener has no connected peer"};
        std::array<unsigned char, 4> header{};
        auto header_result = read_all(header.data(), header.size());
        if (!header_result) return header_result.error();
        const auto size = (static_cast<std::uint32_t>(header[0]) << 24U) |
                          (static_cast<std::uint32_t>(header[1]) << 16U) |
                          (static_cast<std::uint32_t>(header[2]) << 8U) |
                          static_cast<std::uint32_t>(header[3]);
        if (size > max_payload_size) {
            close_channel();
            return Error{EMSGSIZE, "IPC peer declared an oversized message"};
        }
        Message message{};
        try {
            message.payload.resize(size);
        } catch (...) {
            close_channel();
            throw;
        }
        auto payload_result = read_all(
            reinterpret_cast<unsigned char*>(message.payload.data()),
            message.payload.size());
        if (!payload_result) return payload_result.error();
        return message;
    }

private:
    explicit Channel(const int fd) : fd_(fd) {}

    friend auto connect(const std::string&) -> Result<Channel>;
    friend auto listen(const std::string&, std::function<void(Channel&)>)
        -> Result<Channel>;

    [[nodiscard]] inline auto write_all(const unsigned char* data,
                                        std::size_t size) -> Result<void> {
        while (size > 0) {
#if defined(TTYTK_PLATFORM_LINUX)
            const auto count = ::send(fd_, data, size, MSG_NOSIGNAL);
#else
            const auto count = ::send(fd_, data, size, 0);
#endif
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const int error = count < 0 ? errno : EPIPE;
                close_channel();
                return Error{error, std::strerror(error)};
            }
            data += count;
            size -= static_cast<std::size_t>(count);
        }
        return {};
    }

    [[nodiscard]] inline auto read_all(unsigned char* data, std::size_t size)
        -> Result<void> {
        while (size > 0) {
            const auto count = ::recv(fd_, data, size, 0);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) {
                const int error = count < 0 ? errno : ECONNRESET;
                close_channel();
                return Error{error, std::strerror(error)};
            }
            data += count;
            size -= static_cast<std::size_t>(count);
        }
        return {};
    }

    inline void close_channel() noexcept {
        if (fd_ >= 0) static_cast<void>(::close(fd_));
        fd_ = -1;
    }

    int fd_{-1};
    bool listener_{};
    std::function<void(Channel&)> callback_{};
};

/** Connect to a Unix-domain listener by filesystem path. */
[[nodiscard]] inline auto connect(const std::string& path) -> Result<Channel> {
    const auto address = detail::address(path);
    if (!address) return address.error();
    const int descriptor = detail::open_socket();
    if (descriptor < 0) return Error{errno, std::strerror(errno)};
    Channel channel{descriptor};
    auto configured = detail::configure_socket(descriptor);
    if (!configured) return configured.error();
    if (::connect(descriptor, reinterpret_cast<const sockaddr*>(&address.value()),
                  sizeof(sockaddr_un)) < 0) {
        return Error{errno, std::strerror(errno)};
    }
    return channel;
}

/**
 * Bind a nonblocking listener without replacing any existing path.
 * The caller owns removal of the socket path, preferably in a private directory.
 */
[[nodiscard]] inline auto listen(const std::string& path,
                                 std::function<void(Channel&)> callback = {})
    -> Result<Channel> {
    const auto address = detail::address(path);
    if (!address) return address.error();
    const int descriptor = detail::open_socket();
    if (descriptor < 0) return Error{errno, std::strerror(errno)};
    Channel channel{descriptor};
    auto configured = detail::configure_socket(descriptor);
    if (!configured) return configured.error();
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0 ||
        ::bind(descriptor, reinterpret_cast<const sockaddr*>(&address.value()),
               sizeof(sockaddr_un)) < 0 ||
        ::listen(descriptor, SOMAXCONN) < 0) {
        return Error{errno, std::strerror(errno)};
    }
    channel.listener_ = true;
    channel.callback_ = std::move(callback);
    return channel;
}

}  // namespace ttytk::ipc
