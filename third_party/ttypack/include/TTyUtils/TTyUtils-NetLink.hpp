#pragma once

#include <TTyUtils/TTyUtils-EventLoop.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <netdb.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ttyutils::net {

struct Addr {
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
};

namespace detail {

struct Addresses {
    ::addrinfo* entries{};
    ~Addresses() { if (entries) ::freeaddrinfo(entries); }
};

inline auto prepare(const int fd) noexcept -> bool {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return false;
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) return false;
#endif
    return true;
}

inline auto socket(const int family, const int type, const int protocol) noexcept -> int {
#ifdef SOCK_CLOEXEC
    return ::socket(family, type | SOCK_CLOEXEC, protocol);
#else
    const int fd = ::socket(family, type, protocol);
    if (fd >= 0 && ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) { (void)::close(fd); return -1; }
    return fd;
#endif
}

inline auto unix_address(const std::string_view path, ::sockaddr_un& target) noexcept -> ::socklen_t {
    if (path.empty() || path.size() >= sizeof(target.sun_path) ||
        path.find('\0') != std::string_view::npos) return 0;
    target.sun_family = AF_UNIX;
    std::memcpy(target.sun_path, path.data(), path.size());
    const auto size = static_cast<::socklen_t>(offsetof(::sockaddr_un, sun_path) + path.size() + 1);
#ifdef __APPLE__
    target.sun_len = static_cast<unsigned char>(size);
#endif
    return size;
}

}  // namespace detail

/// Owning stream socket with non-blocking I/O and a bounded output queue.
class Link {
public:
    Link() = default;
    explicit Link(const int fd) : fd_(fd) { if (fd_ >= 0 && !detail::prepare(fd_)) close(); }
    Link(const Link&) = delete;
    auto operator=(const Link&) -> Link& = delete;
    Link(Link&& other) noexcept { *this = std::move(other); }
    auto operator=(Link&& other) noexcept -> Link& {
        if (this != &other) {
            close();
            other.detach();
            fd_ = std::exchange(other.fd_, -1);
            pending_ = std::exchange(other.pending_, {});
            sent_ = std::exchange(other.sent_, 0);
            callback_ = std::move(other.callback_);
        }
        return *this;
    }
    ~Link() { close(); }

    /// Resolve and connect synchronously. The returned link performs non-blocking I/O.
    [[nodiscard]] static auto connect(const Addr& address) -> std::optional<Link> {
        if (address.host.empty() || address.host.find('\0') != std::string::npos) return std::nullopt;
        const auto service = std::to_string(address.port);
        ::addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        detail::Addresses addresses;
        if (::getaddrinfo(address.host.c_str(), service.c_str(), &hints, &addresses.entries) != 0)
            return std::nullopt;
        for (auto* item = addresses.entries; item; item = item->ai_next) {
            if (auto link = open(item->ai_family, item->ai_socktype, item->ai_protocol,
                                 item->ai_addr, item->ai_addrlen)) return link;
        }
        return std::nullopt;
    }
    [[nodiscard]] static auto connect_unix(const std::string_view path) -> std::optional<Link> {
        ::sockaddr_un target{};
        const auto size = detail::unix_address(path, target);
        return size == 0 ? std::nullopt :
            open(AF_UNIX, SOCK_STREAM, 0, reinterpret_cast<const ::sockaddr*>(&target), size);
    }

    [[nodiscard]] auto fd() const noexcept -> int { return fd_; }
    /// Returns bytes read, zero for EOF, or nullopt with errno set.
    [[nodiscard]] auto read(void* buffer, const std::size_t size) -> std::optional<std::size_t> {
        ssize_t count;
        do { count = ::recv(fd_, buffer, size, 0); } while (count < 0 && errno == EINTR);
        if (count < 0) return std::nullopt;
        return static_cast<std::size_t>(count);
    }
    /// Queue the whole input, or reject it if the queue has reached its size limit.
    [[nodiscard]] auto write(const std::string_view data) -> bool {
        if (fd_ < 0 || data.size() > queue_limit - pending_bytes()) return false;
        if (sent_ != 0) { pending_.erase(0, sent_); sent_ = 0; }
        pending_.append(data);
        (void)flush();
        return fd_ >= 0;
    }
    [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t { return pending_.size() - sent_; }
    /// Retry queued data; false means blocked or closed.
    [[nodiscard]] auto flush() -> bool {
        if (fd_ < 0) return false;
        while (pending_bytes() != 0) {
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto count = ::send(fd_, pending_.data() + sent_, pending_bytes(), flags);
            if (count > 0) sent_ += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR) continue;
            else if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
                watch_.set_events(loop::Readable | loop::Writable);
                return false;
            }
            else { close(); return false; }
        }
        pending_.clear();
        sent_ = 0;
        watch_.set_events(loop::Readable);
        return true;
    }
    void close() noexcept {
        detach();
        if (fd_ >= 0) (void)::close(std::exchange(fd_, -1));
        pending_.clear();
        sent_ = 0;
    }
    void on_data(std::function<void(std::string)> callback) { callback_ = std::move(callback); }
    /// Register input delivery and automatically flush queued output on readiness.
    /// Moving the link detaches it from the loop.
    void attach(loop::Loop& loop) {
        detach();
        if (fd_ >= 0) watch_ = loop.watch(fd_, pending_bytes() ? loop::Readable | loop::Writable : loop::Readable,
            [this](const loop::Event& event) {
                if (event.kind == loop::Event::Kind::writable) (void)flush();
                else pump();
            });
    }
    void detach() noexcept { watch_.cancel(); }
    void pump() {
        char bytes[8192];
        const auto count = read(bytes, sizeof(bytes));
        if (!count) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) close();
        } else if (*count == 0) close();
        else if (auto callback = callback_) callback(std::string(bytes, *count));
    }

private:
    static auto open(const int family, const int type, const int protocol, const ::sockaddr* address,
                     const ::socklen_t size) -> std::optional<Link> {
        const int fd = detail::socket(family, type, protocol);
        if (fd < 0) return std::nullopt;
        if (::connect(fd, address, size) == 0) {
            Link link{fd};
            if (link.fd() >= 0) return link;
        } else (void)::close(fd);
        return std::nullopt;
    }

    static constexpr std::size_t queue_limit = 1024 * 1024;
    int fd_{-1};
    std::string pending_;
    std::size_t sent_{};
    std::function<void(std::string)> callback_{};
    loop::FdWatch watch_;
};

/// Owning TCP or Unix listener; accept never blocks.
class Listener {
public:
    Listener() = default;
    explicit Listener(const int fd) : fd_(fd) { if (fd_ >= 0 && !detail::prepare(fd_)) close(); }
    Listener(const Listener&) = delete;
    auto operator=(const Listener&) -> Listener& = delete;
    Listener(Listener&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    auto operator=(Listener&& other) noexcept -> Listener& {
        if (this != &other) { close(); fd_ = std::exchange(other.fd_, -1); }
        return *this;
    }
    ~Listener() { close(); }
    /// Bind exactly the requested address; an empty host explicitly requests a wildcard.
    [[nodiscard]] static auto bind(const Addr& address) -> std::optional<Listener> {
        if (address.host.find('\0') != std::string::npos) return std::nullopt;
        const auto service = std::to_string(address.port);
        ::addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        detail::Addresses addresses;
        if (::getaddrinfo(address.host.empty() ? nullptr : address.host.c_str(), service.c_str(),
                          &hints, &addresses.entries) != 0) return std::nullopt;
        for (auto* item = addresses.entries; item; item = item->ai_next) {
            if (auto listener = open(item->ai_family, item->ai_socktype, item->ai_protocol,
                                     item->ai_addr, item->ai_addrlen)) return listener;
        }
        return std::nullopt;
    }
    /// Bind a filesystem socket. The caller owns unlinking its pathname.
    [[nodiscard]] static auto bind_unix(const std::string_view path) -> std::optional<Listener> {
        ::sockaddr_un target{};
        const auto size = detail::unix_address(path, target);
        return size == 0 ? std::nullopt :
            open(AF_UNIX, SOCK_STREAM, 0, reinterpret_cast<const ::sockaddr*>(&target), size);
    }
    [[nodiscard]] auto accept() -> std::optional<Link> {
        int fd;
#ifdef __linux__
        do { fd = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK); }
        while (fd < 0 && errno == EINTR);
#else
        do { fd = ::accept(fd_, nullptr, nullptr); } while (fd < 0 && errno == EINTR);
#endif
        if (fd < 0) return std::nullopt;
        Link link{fd};
        return link.fd() < 0 ? std::nullopt : std::optional<Link>{std::move(link)};
    }
    [[nodiscard]] auto fd() const noexcept -> int { return fd_; }
    void close() noexcept { if (fd_ >= 0) (void)::close(std::exchange(fd_, -1)); }

private:
    static auto open(const int family, const int type, const int protocol, const ::sockaddr* address,
                     const ::socklen_t size) -> std::optional<Listener> {
        const int fd = detail::socket(family, type, protocol);
        if (fd < 0) return std::nullopt;
        Listener listener{fd};
        if (listener.fd() < 0) return std::nullopt;
        if (family != AF_UNIX) {
            const int enabled = 1;
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
        }
        if (::bind(fd, address, size) != 0 || ::listen(fd, 16) != 0) return std::nullopt;
        return listener;
    }
    int fd_{-1};
};

/// Prerequisite marker; no TLS backend is provided.
struct TlsContext { bool enabled{}; };

class Resolver {
public:
    /// Resolve unique numerical endpoints in the platform's preferred order.
    [[nodiscard]] static auto resolve(const std::string_view host, const std::uint16_t port) -> std::vector<Addr> {
        std::vector<Addr> result;
        if (host.empty() || host.find('\0') != std::string_view::npos) return result;
        ::addrinfo hints{};
        hints.ai_socktype = SOCK_STREAM;
        detail::Addresses addresses;
        const auto service = std::to_string(port);
        if (::getaddrinfo(std::string(host).c_str(), service.c_str(), &hints, &addresses.entries) != 0)
            return result;
        for (auto* item = addresses.entries; item; item = item->ai_next) {
            char numeric[NI_MAXHOST];
            if (::getnameinfo(item->ai_addr, item->ai_addrlen, numeric, sizeof(numeric),
                              nullptr, 0, NI_NUMERICHOST) == 0 &&
                std::none_of(result.begin(), result.end(), [&](const Addr& addr) { return addr.host == numeric; }))
                result.push_back({numeric, port});
        }
        return result;
    }
};

}  // namespace ttyutils::net
