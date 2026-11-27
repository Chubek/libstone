#include <TTyTk/TTyTk-ServerNetUtils.hpp>
#include <cassert>

int main() {
    ttytk::net::TlsServer tls{0};
    assert(!tls.available());
    ttytk::net::TcpServer original{0};
    int seen = -1;
    original.on_client([&](int fd) { seen = fd; });
    ttytk::net::TcpServer moved{std::move(original)};
    assert(!original.port());
    const auto port = moved.port();
    if (!port) return 1;
    const auto peer = ::socket(AF_INET, SOCK_STREAM, 0);
    if (peer < 0) return 1;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(port.value());
    if (::connect(peer, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0) return 1;
    const auto accepted = ttytk::net::accept_terminal(moved);
    if (!accepted) return 1;
    assert(accepted.value() == seen);
    assert((::fcntl(accepted.value(), F_GETFD) & FD_CLOEXEC) != 0);
    ::close(accepted.value());
    ::close(peer);
}
