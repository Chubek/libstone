#include <TTyTk/TTyTk-SerialPort.hpp>

#include <cassert>
#include <cstdlib>

int main() {
    const auto serial = ttytk::Serial::open("/no/such/serial/device", 9600);
    assert(!serial);
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    assert(master >= 0);
    assert(::grantpt(master) == 0 && ::unlockpt(master) == 0);
    const char* const slave = ::ptsname(master);
    assert(slave);
    auto attached = ttytk::Serial::open(slave, 2400);
    assert(attached);
    auto& port = attached.value();
    assert(!port.is_hardware());
    assert(::fcntl(port.native_fd(), F_GETFD) & FD_CLOEXEC);
    termios settings{};
    assert(::tcgetattr(port.native_fd(), &settings) == 0);
    assert(::cfgetospeed(&settings) == B2400);
    assert((settings.c_lflag & (ECHO | ICANON)) == 0);
    assert(!port.set_baud(12345));
    assert(!port.set_params(9));
    assert(port.set_handshake(ttytk::Serial::Handshake::XonXoff));
    assert(port.set_params(8, 'N', 2));
    assert(::tcgetattr(port.native_fd(), &settings) == 0);
    assert((settings.c_iflag & (IXON | IXOFF)) == (IXON | IXOFF));
    assert((settings.c_iflag & IXANY) == 0);
    assert(settings.c_cc[VSTART] == 0x11 && settings.c_cc[VSTOP] == 0x13);
    assert(settings.c_cflag & CSTOPB);
    assert(::close(master) == 0);
}
