#include <TTyUtils/TTyUtils-Telnet.hpp>
#include <cassert>

int main() {
    using namespace ttyutils::telnet;
    Negotiator parser;
    parser.enable(Option::Echo);
    assert(parser.feed("hello\xff").data == "hello");
    assert(parser.feed("\xff").data == "\xff");
    assert(parser.feed("\xff\xfb").reply.empty());
    assert(parser.feed("\x01").reply == "\xff\xfd\x01");
    assert(parser.feed("\xff\xfb\x01").reply.empty());
    assert(parser.feed("\xff\xfc\x01").reply == "\xff\xfe\x01");
    assert(parser.feed("\xff\xfb\xf1").reply == "\xff\xfe\xf1");
    assert(parser.feed("\xff\xfb\xf1").reply.empty());
    assert(Negotiator::escape("x\xff") == "x\xff\xff");

    parser.enable(Option::Ttype);
    parser.terminal_type("xterm");
    assert(parser.feed("\xff\xfd\x18").reply == "\xff\xfb\x18");
    assert(parser.feed("\xff\xfa\x18\x01\xff").reply.empty());
    assert(parser.feed("\xf0").reply == std::string("\xff\xfa\x18\0xterm\xff\xf0", 11));

    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    Session session{ttyutils::net::Link{sockets[0]}};
    ttyutils::net::Link peer{sockets[1]};
    session.enable(Option::Naws);
    assert(!session.send_window_size(255, 511));
    assert(session.receive("\xff\xfd\x1f").empty());
    char reply[64];
    assert(peer.read(reply, sizeof(reply)) == 3);
    assert(std::string_view(reply, 3) == "\xff\xfb\x1f");
    assert(session.send_window_size(255, 511));
    assert(peer.read(reply, sizeof(reply)) == 11);
    assert(std::string_view(reply, 11) == std::string_view("\xff\xfa\x1f\0\xff\xff\x01\xff\xff\xff\xf0", 11));
}
