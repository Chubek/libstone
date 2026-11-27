#include <TTyTk/TTyTk-P2PNetUtils.hpp>
#include <cassert>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main() {
    ttytk::p2p::Discovery discovery;
    discovery.add("peer");
    assert(discovery.peers().size() == 1);

    char directory[]{"/tmp/ttytk-p2p-XXXXXX"};
    if (!::mkdtemp(directory)) return 1;
    const auto path = std::string{directory} + "/socket";
    auto listener = ttytk::ipc::listen(path);
    auto client = ttytk::ipc::connect(path);
    auto server = listener ? listener.value().accept() :
                             ttytk::Result<ttytk::ipc::Channel>{listener.error()};
    ::unlink(path.c_str());
    ::rmdir(directory);
    if (!client || !server) return 1;
    ttytk::p2p::Connection from{std::move(client).value()}, to{std::move(server).value()};
    struct Buffer {
        struct Cell { std::string_view grapheme; bool continuation; };
        Cell cells[4]{{"界", false}, {"", true}, {"a", false}, {" ", false}};
        auto rows() const { return 2; }
        auto columns() const { return 2; }
        auto at(std::size_t row, std::size_t column) const -> const Cell& { return cells[row * 2 + column]; }
    };
    if (!ttytk::p2p::sync_buffer(Buffer{}, from)) return 1;
    const auto received = to.receive();
    assert(received && received.value().payload == "界\na ");
    if (!ttytk::p2p::sync_buffer(Buffer{}, to, std::string_view{"x\0y", 3})) return 1;
    const auto binary = from.receive();
    assert(binary && binary.value().payload == std::string("x\0y", 3));
}
