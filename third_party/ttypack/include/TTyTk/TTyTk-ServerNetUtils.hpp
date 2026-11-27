#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-IPCTools.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <fcntl.h>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace ttytk::net {

/** A synchronous plain TCP listener; no authentication or terminal protocol is implied. */
class TcpServer {
public:
    explicit TcpServer(const std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            error_ = errno;
            return;
        }
        const int reuse = 1;
        static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0 ||
            ::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd_, SOMAXCONN) != 0) {
            error_ = errno;
            static_cast<void>(::close(fd_));
            fd_ = -1;
        }
    }

    TcpServer(const TcpServer&) = delete;
    auto operator=(const TcpServer&) -> TcpServer& = delete;
    TcpServer(TcpServer&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), error_(std::exchange(other.error_, EBADF)),
          callback_(std::move(other.callback_)) {}
    auto operator=(TcpServer&& other) noexcept -> TcpServer& {
        if (this != &other) {
            if (fd_ >= 0) static_cast<void>(::close(fd_));
            fd_ = std::exchange(other.fd_, -1);
            error_ = std::exchange(other.error_, EBADF);
            callback_ = std::move(other.callback_);
        }
        return *this;
    }
    inline ~TcpServer() { if (fd_ >= 0) static_cast<void>(::close(fd_)); }

    /** Install a callback borrowing each accepted descriptor during accept_terminal(). */
    inline void on_client(std::function<void(int)> callback) {
        callback_ = callback ? std::make_shared<decltype(callback)>(std::move(callback)) : nullptr;
    }

    /** Return the bound port, including an OS-assigned port when constructed with zero. */
    [[nodiscard]] inline auto port() const -> Result<std::uint16_t> {
        if (fd_ < 0) return Error{error_, std::strerror(error_)};
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return Error{errno, std::strerror(errno)};
        return ntohs(address.sin_port);
    }

private:
    friend auto accept_terminal(TcpServer&) -> Result<int>;
    int fd_{-1};
    int error_{EBADF};
    std::shared_ptr<std::function<void(int)>> callback_{};
};

class TlsServer {
public:
    explicit TlsServer(const std::uint16_t) {}
    // NOTE(agent): TLS cannot be safely implemented without a TLS dependency,
    // which this project prohibits. The type remains an explicit unavailable stub.
    [[nodiscard]] inline auto available() const noexcept -> bool { return false; }
};

/** Accept one transport connection; the returned descriptor belongs to the caller. */
[[nodiscard]] inline auto accept_terminal(TcpServer& server) -> Result<int> {
    if (server.fd_ < 0) return Error{server.error_, std::strerror(server.error_)};
    int client;
    do {
        client = ::accept(server.fd_, nullptr, nullptr);
    } while (client < 0 && errno == EINTR);
    if (client < 0) return Error{errno, std::strerror(errno)};
    if (::fcntl(client, F_SETFD, FD_CLOEXEC) != 0) {
        const auto error = errno;
        static_cast<void>(::close(client));
        return Error{error, std::strerror(error)};
    }
    try {
        const auto callback = server.callback_;
        if (callback) (*callback)(client);
    } catch (...) {
        static_cast<void>(::close(client));
        throw;
    }
    return client;
}

}  // namespace ttytk::net
