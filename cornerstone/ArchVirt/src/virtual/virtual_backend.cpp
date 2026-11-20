#include "virtual_backend.hpp"
#include "machine/runtime.hpp"
#include "cli/boot_display.hpp"
#include <chrono>
#include <iostream>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace domvirt::virtualization {
VmResult run_virtual_backend(const VirtualBootOptions& options, BootDisplay& display,
                             const volatile bool& running) {
    machine::BootOptions boot;
    boot.machine = options.machine;
    boot.backend = "software";
    boot.kernel_path = options.kernel_path; boot.image_path = options.initrd_path;
    boot.firmware_path = options.firmware_path; boot.memory_mb = options.memory_mb;
    boot.bundled_domix = options.use_domix; boot.domios = options.use_domios;
    machine::Runtime runtime;
    auto result = runtime.initialize(boot, [&](bool write, std::uint8_t value) -> std::uint8_t {
        if (write) { std::cout.put(static_cast<char>(value)).flush(); return 0; }
        if (display.try_read_input(value)) return value;
        pollfd input{STDIN_FILENO, POLLIN, 0};
        return poll(&input, 1, 1) > 0 && (input.revents & POLLIN) && read(STDIN_FILENO, &value, 1) == 1 ? value : 0;
    }, [&](std::span<const std::uint8_t> cells) { display.set_guest_text_buffer(cells.data(), 80, 25, 160); });
    if (!result) return result;
    result = runtime.start();
    if (!result) return result;
    while (running && runtime.running() && !display.closed()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    runtime.stop(); runtime.join();
    return runtime.result();
}
}
