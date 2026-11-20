#include "machine/runtime.hpp"
#include "machine/profile.hpp"
#include "cli/boot_display.hpp"
#include "bindlet_bridge/control_socket.hpp"
#include "bootstrap.hpp"
#include "testing/test_session.hpp"
#include <nlohmann/json.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

using namespace domvirt;
namespace {
volatile std::sig_atomic_t running = 1;
void stop_signal(int) { running = 0; }
const char* env(const char* primary, const char* alias) {
    if (const auto* value = std::getenv(primary); value && *value) return value;
    if (const auto* value = std::getenv(alias); value && *value) return value;
    return nullptr;
}
std::uint64_t number(const char* text) {
    std::uint64_t value;
    auto [end, error] = std::from_chars(text, text + std::strlen(text), value);
    if (error != std::errc{} || *end) throw std::invalid_argument(std::string("Invalid integer: ") + text);
    return value;
}
struct Terminal {
    termios saved{};
    bool changed{false};
    explicit Terminal(bool enabled = true) {
        if (enabled && isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
            auto mode = saved; mode.c_lflag &= ~(ICANON | ECHO); mode.c_cc[VMIN] = 0; mode.c_cc[VTIME] = 0;
            changed = tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0;
        }
    }
    ~Terminal() { if (changed) tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
};
void usage(const char* name) {
    std::cout << "Usage: " << name << " [options]\n"
        "  --machine x86_64|aarch64|riscv64   Select a DomIOS PC machine\n"
        "  --archml PATH             Use a validated ArchML machine profile\n"
        "  --list-machines           List supported machines\n"
        "  --backend auto|software|kvm (default: auto)\n"
        "  -V, --virtual             Select software execution\n"
        "  --headless                Use the terminal console only\n"
        "  --max-instructions N      Software execution limit (0: unlimited)\n"
        "  -m, --memory MB           Guest RAM, 40..1024 MiB (default: 64)\n"
        "  -k, --kernel PATH         Identity-mapped ELF64 guest kernel\n"
        "  -i, --initrd PATH         Guest filesystem image\n"
        "  -o, --os PATH             Domix packed system image\n"
        "  -b, --bios PATH           Firmware image (DomIOS or B1 ROM by boot mode)\n"
        "  --boot-mode direct|bios   Direct ELF or legacy PC ROM boot\n"
        "  --disk PATH              BIOS raw ATA disk (private, volatile writes)\n"
        "  -c, --cmdline TEXT        Guest boot command line (up to 2048 bytes)\n"
        "  --[no-]domix              Enable/disable bundled Domix\n"
        "  --[no-]domios             Enable/disable DomIOS firmware\n"
        "  --control PATH            Unix control socket\n"
        "  --test-server PATH        Headless IPC server; boot/control guests via JSON\n"
        "  -h, --help; -v, --version\n";
}
int transfer(int argc, char** argv) {
    if (argc != 5 || (std::strcmp(argv[2], "push") && std::strcmp(argv[2], "pull"))) {
        std::cerr << "Usage: archvirt transfer push|pull SOURCE DESTINATION\n"; return 1;
    }
    const bool push = std::strcmp(argv[2], "push") == 0;
    nlohmann::json request{{"execute", push ? "file_push" : "file_pull"},
        {"host_path", push ? argv[3] : argv[4]}, {"guest_path", push ? argv[4] : argv[3]}};
    auto* path = env("DOMVIRT_CONTROL", "DOMVIRT_CONTROL_SOCK");
    if (!path) path = "/tmp/domvirt.sock";
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(address.sun_path)) throw std::invalid_argument("Control path too long");
    std::strcpy(address.sun_path, path);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("Cannot open control socket");
    struct Socket { int fd; ~Socket() { close(fd); } } socket_guard{fd};
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        throw std::runtime_error("Cannot connect to control socket");
    const auto payload = request.dump();
    std::size_t written = 0;
    while (written < payload.size()) {
        auto n = send(fd, payload.data() + written, payload.size() - written, MSG_NOSIGNAL);
        if (n <= 0) throw std::runtime_error("Control request failed");
        written += n;
    }
    shutdown(fd, SHUT_WR);
    std::string response; char buffer[4096];
    for (;;) {
        auto n = read(fd, buffer, sizeof(buffer)); if (!n) break;
        if (n < 0 || response.size() > 65536) throw std::runtime_error("Invalid control response");
        response.append(buffer, n);
    }
    std::cout << response << '\n';
    return nlohmann::json::parse(response).contains("error") ? 1 : 0;
}
}

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::strcmp(argv[1], "transfer") == 0) return transfer(argc, argv);
        machine::BootOptions options;
        bool headless = !std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY");
        std::string control_path;
        bool test_server = false, backend_selected = false;
        if (auto* p = env("ARCHVIRT_MACHINE", "DOMVIRT_MACHINE")) options.machine = p;
        if (auto* p = env("DOMVIRT_KERNEL", "DOMVIRT_KERNEL_PATH")) { options.kernel_path = p; options.bundled_domix = false; }
        if (auto* p = env("DOMVIRT_INITRD", "DOMVIRT_INITRD_PATH")) options.image_path = p;
        if (auto* p = env("DOMVIRT_OS", "DOMVIRT_OS_PATH")) { options.image_path = p; options.bundled_domix = true; }
        if (auto* p = env("DOMVIRT_BIOS", "DOMVIRT_BIOS_PATH")) options.firmware_path = p;
        if (auto* p = env("DOMVIRT_CONTROL", "DOMVIRT_CONTROL_SOCK")) control_path = p;
        const option long_options[] = {
            {"machine", required_argument, nullptr, 1000}, {"archml", required_argument, nullptr, 1001},
            {"backend", required_argument, nullptr, 1002}, {"headless", no_argument, nullptr, 1003},
            {"max-instructions", required_argument, nullptr, 1004}, {"list-machines", no_argument, nullptr, 1005},
            {"domix", no_argument, nullptr, 1006}, {"no-domix", no_argument, nullptr, 1007},
            {"domios", no_argument, nullptr, 1008}, {"no-domios", no_argument, nullptr, 1009},
            {"control", required_argument, nullptr, 1010}, {"kernel", required_argument, nullptr, 'k'},
            {"test-server", required_argument, nullptr, 1011},
            {"boot-mode", required_argument, nullptr, 1012}, {"disk", required_argument, nullptr, 1013},
            {"initrd", required_argument, nullptr, 'i'}, {"os", required_argument, nullptr, 'o'},
            {"bios", required_argument, nullptr, 'b'}, {"memory", required_argument, nullptr, 'm'},
            {"cmdline", required_argument, nullptr, 'c'}, {"virtual", no_argument, nullptr, 'V'},
            {"help", no_argument, nullptr, 'h'}, {"version", no_argument, nullptr, 'v'}, {nullptr, 0, nullptr, 0}};
        int opt;
        while ((opt = getopt_long(argc, argv, "k:i:o:b:m:c:Vhv", long_options, nullptr)) != -1) {
            switch (opt) {
            case 1000: options.machine = optarg; break;
            case 1001: options.archml_path = optarg; break;
            case 1002: options.backend = optarg; backend_selected = true; break;
            case 1003: headless = true; break;
            case 1004: options.instruction_limit = number(optarg); break;
            case 1005:
                for (auto name : {"x86_64", "aarch64", "riscv64"}) {
                    const auto profile = machine::Profile::load(name);
                    std::cout << profile.name << "  ArchML=" << profile.isa.name()
                              << "  DomIOS PC  software" << (profile.native_kvm() ? ", native KVM" : "") << '\n';
                }
                return 0;
            case 1006: options.bundled_domix = true; break;
            case 1007: options.bundled_domix = false; break;
            case 1008: options.domios = true; break;
            case 1009: options.domios = false; break;
            case 1010: control_path = optarg; break;
            case 1011: control_path = optarg; test_server = true; break;
            case 1012: options.boot_mode = optarg; break;
            case 1013: options.disk_path = optarg; break;
            case 'k': options.kernel_path = optarg; options.bundled_domix = false; break;
            case 'i': options.image_path = optarg; break;
            case 'o': options.image_path = optarg; options.bundled_domix = true; break;
            case 'b': options.firmware_path = optarg; options.domios = true; break;
            case 'm': options.memory_mb = number(optarg); break;
            case 'c': options.cmdline = optarg; break;
            case 'V': options.backend = "software"; backend_selected = true; break;
            case 'h': usage(argv[0]); return 0;
            case 'v': std::cout << "ArchVirt " << DOMVIRT_VERSION << '\n'; return 0;
            default: usage(argv[0]); return 1;
            }
        }
        if (optind != argc) throw std::invalid_argument("Unexpected positional argument");
        if (test_server) {
            if (control_path.empty()) throw std::invalid_argument("--test-server requires a socket path");
            headless = true;
            if (!backend_selected) options.backend = "software";
        }
        signal(SIGINT, stop_signal); signal(SIGTERM, stop_signal);
        BootDisplay display(headless);
        Terminal terminal(!test_server);
        auto serial = [&](bool write, std::uint8_t value) -> std::uint8_t {
            if (write) { std::cout.put(static_cast<char>(value)).flush(); return 0; }
            if (display.try_read_input(value)) return value;
            pollfd input{STDIN_FILENO, POLLIN, 0};
            if (poll(&input, 1, 1) > 0 && (input.revents & POLLIN) && read(STDIN_FILENO, &value, 1) == 1) return value;
            return 0;
        };
        testing::TestSession session(options, test_server,
            test_server ? machine::Runtime::Serial{} : serial,
            [&](std::span<const std::uint8_t> cells) {
                if (!headless) display.set_guest_text_buffer(cells.data(), 80, 25, 160);
            });
        bindlet_bridge::ControlSocket control;
        if (!control_path.empty()) {
            session.attach(control);
            const auto result = control.start(control_path);
            if (!result) throw std::runtime_error(result.message);
        }
        if (test_server) {
            std::cout << "ArchVirt IPC ready: " << control_path << std::endl;
            while (running && !session.quitting() && control.is_running())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            control.stop(); session.stop();
            return 0;
        }
        display.set_status("DomIOS: preparing machine", 0.1f);
        if (options.boot_mode == "direct" && options.domios && !headless) {
            domios::SplashFrame splash; std::string error;
            if (domios::Bootstrap{}.render_splash(splash, error)) {
                display.set_guest_framebuffer(splash.pixels.data(), splash.geometry.width,
                    splash.geometry.height, splash.geometry.pitch, static_cast<unsigned>(splash.format));
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                display.set_guest_framebuffer(nullptr, 0, 0, 0, 0);
            }
        }
        std::cout << "ArchVirt: " << options.machine << " / " << options.backend
                  << (options.boot_mode == "bios" ? " / PC BIOS\n" : options.domios ? " / DomIOS handoff\n" : "\n");
        display.set_status("Running " + options.machine, 1);
        auto result = session.boot(options);
        if (!result) throw std::runtime_error(result.message);
        while (running && session.running() && !session.quitting() && !display.closed()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        control.stop(); session.stop(); display.stop();
        result = session.result();
        if (!result) throw std::runtime_error(result.message);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ArchVirt: " << error.what() << '\n'; return 1;
    }
}
