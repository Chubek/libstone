#include <TTyUtils/TTyUtils-SSH.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::ssh;
    KnownHosts known;
    known.add("host.example");
    assert(known.contains("host.example"));
    assert(!known.contains("host") && !known.contains(""));
    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    Session session{ttyutils::net::Link{sockets[0]}};
    ttyutils::net::Link peer{sockets[1]};
    assert(!session.auth(Auth::password_auth("user", "password")));
    assert(!session.authenticated());
    assert(!session.open_channel().write("private input"));
    char byte;
    assert(!peer.read(&byte, 1));
    int channel_sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, channel_sockets) == 0);
    Channel channel{ttyutils::net::Link{channel_sockets[0]}};
    ttyutils::net::Link channel_peer{channel_sockets[1]};
    assert(!channel.write("private input") && channel_peer.read(&byte, 1) == 0);
}
