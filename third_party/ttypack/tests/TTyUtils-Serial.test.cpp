#include <TTyUtils/TTyUtils-Serial.hpp>
#include <cassert>
#include <chrono>
#include <cstdlib>

int main() {
    using namespace ttyutils::serial;
    const auto master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master >= 0 && ::grantpt(master) == 0 && ::unlockpt(master) == 0);
    auto port = Port::open(::ptsname(master), {.baud = 9600, .flow = Flow::Software, .stop_bits = 2});
    assert(port);
    termios state{};
    assert(::tcgetattr(port->fd(), &state) == 0);
    assert(::cfgetispeed(&state) == B9600 && ::cfgetospeed(&state) == B9600);
    assert((state.c_iflag & (IXON | IXOFF)) == (IXON | IXOFF));
    assert((state.c_cflag & CSTOPB) != 0);
    assert(!port->configure({.baud = 123456}));
    assert(port->configure({.baud = 19200}));
    assert(::tcgetattr(port->fd(), &state) == 0);
    assert((state.c_iflag & (IXON | IXOFF)) == 0 && (state.c_cflag & CSTOPB) == 0);
    ttyutils::loop::Loop loop;
    std::string received;
    port->on_data([&](std::string data) { received += data; });
    port->attach(loop);
    assert(::write(master, "line", 4) == 4);
    loop.run_once(std::chrono::milliseconds(100));
    assert(received == "line");
    port.reset();
    loop.run_once(std::chrono::milliseconds(0));
    (void)::close(master);
}
