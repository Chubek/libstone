#include <TTyUtils/TTyUtils-NetLink.hpp>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <netinet/in.h>

int main() {
    using namespace ttyutils::net;
    auto listener = Listener::bind({"127.0.0.1", 0});
    assert(listener && !listener->accept());
    ::sockaddr_in bound{};
    ::socklen_t size = sizeof(bound);
    assert(::getsockname(listener->fd(), reinterpret_cast<::sockaddr*>(&bound), &size) == 0);
    assert(bound.sin_family == AF_INET && ntohl(bound.sin_addr.s_addr) == INADDR_LOOPBACK);
    const auto port = ntohs(bound.sin_port);
    assert(Resolver::resolve("127.0.0.1", port).front().host == "127.0.0.1");
    auto client = Link::connect({"127.0.0.1", port});
    auto server = listener->accept();
    assert(client && server);
    assert((::fcntl(client->fd(), F_GETFL) & O_NONBLOCK) != 0);
    assert((::fcntl(server->fd(), F_GETFD) & FD_CLOEXEC) != 0);
    ttyutils::loop::Loop loop;
    std::string received;
    server->on_data([&](std::string data) { received += data; });
    server->attach(loop);
    client->attach(loop);
    const int buffer_size = 4096;
    assert(::setsockopt(client->fd(), SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0);
    const std::string payload(128 * 1024, 'x');
    assert(client->write(payload));
    assert(client->pending_bytes() != 0);
    for (int i = 0; i < 300 && received.size() < payload.size(); ++i)
        loop.run_once(std::chrono::milliseconds(10));
    assert(received == payload && client->pending_bytes() == 0);
    server.reset();
    loop.run_once(std::chrono::milliseconds(100));
    assert(client->fd() == -1 && !client->write("closed"));

    char directory[] = "/tmp/ttypack-net-XXXXXX";
    assert(::mkdtemp(directory));
    const std::string path = std::string(directory) + "/s";
    auto unix_listener = Listener::bind_unix(path);
    assert(unix_listener);
    auto first = Link::connect_unix(path);
    auto second = unix_listener->accept();
    assert(first && second && first->write("ok"));
    char bytes[2];
    assert(second->read(bytes, sizeof(bytes)) == 2 && std::string_view(bytes, 2) == "ok");
    unix_listener.reset();
    assert(::unlink(path.c_str()) == 0 && ::rmdir(directory) == 0);
}
