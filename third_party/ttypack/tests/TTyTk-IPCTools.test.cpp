#include <TTyTk/TTyTk-IPCTools.hpp>

#include <cassert>
#include <cstdlib>
#include <fstream>

int main() {
    const auto channel = ttytk::ipc::connect("/no/such/ttytk.sock");
    assert(!channel);
    char directory[] = "/tmp/ttytk-ipc-test-XXXXXX";
    assert(::mkdtemp(directory) != nullptr);
    const auto path = std::string{directory} + "/sock";
    std::ofstream{path} << "keep";
    const auto occupied = ttytk::ipc::listen(path);
    assert(!occupied && occupied.error().code == EADDRINUSE);
    std::string contents;
    std::ifstream{path} >> contents;
    assert(contents == "keep");
    assert(::unlink(path.c_str()) == 0);

    int accepted_count = 0;
    auto listener = ttytk::ipc::listen(path, [&](ttytk::ipc::Channel& peer) {
        assert(peer.native_fd() >= 0);
        ++accepted_count;
    });
    assert(listener && accepted_count == 0);
    const auto empty_queue = listener.value().accept();
    assert(!empty_queue);
    assert(empty_queue.error().code == EAGAIN || empty_queue.error().code == EWOULDBLOCK);
    auto client = ttytk::ipc::connect(path);
    assert(client);
    auto peer = listener.value().accept();
    assert(peer && accepted_count == 1);
    const std::string binary{"a\0b", 3};
    assert(client.value().send({binary}));
    assert(peer.value().receive().value().payload == binary);
    assert(peer.value().send({}));
    assert(client.value().receive().value().payload.empty());
    client.value() = ttytk::ipc::Channel{};
    const auto closed = peer.value().receive();
    assert(!closed && closed.error().code == ECONNRESET);

    auto malformed = ttytk::ipc::connect(path);
    auto receiver = listener.value().accept();
    assert(malformed && receiver);
    const unsigned char oversized[4]{0xff, 0xff, 0xff, 0xff};
    assert(::send(malformed.value().native_fd(), oversized, sizeof(oversized), 0) == 4);
    const auto result = receiver.value().receive();
    assert(!result && result.error().code == EMSGSIZE);
    assert(receiver.value().native_fd() == -1);
    assert(::unlink(path.c_str()) == 0);
    assert(::rmdir(directory) == 0);
}
