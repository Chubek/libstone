#pragma once
#include "machine/runtime.hpp"
#include "bindlet_bridge/control_socket.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <deque>
#include <mutex>

namespace domvirt::testing {

// Owns the VM lifecycle; CPU callbacks use a separate lock from IPC commands.
class TestSession {
public:
    TestSession(machine::BootOptions defaults, bool managed,
                machine::Runtime::Serial serial = {}, machine::Runtime::Console console = {});
    ~TestSession();
    void attach(bindlet_bridge::ControlSocket& socket);
    VmResult boot(const machine::BootOptions& options, bool paused = false);
    void stop();
    bool running() const;
    bool quitting() const { return quitting_.load(); }
    VmResult result() const;
private:
    VmResult boot_locked(const machine::BootOptions& options, bool paused);
    nlohmann::json status_locked() const;
    std::string handle(const std::string& command, const std::string& request);
    std::uint8_t serial(bool write, std::uint8_t value);
    void console(std::span<const std::uint8_t> cells);

    mutable std::mutex mutex_, io_mutex_;
    machine::Runtime runtime_;
    machine::BootOptions defaults_, configured_;
    bool managed_, configured_valid_{false};
    machine::Runtime::Serial external_serial_;
    machine::Runtime::Console external_console_;
    std::deque<std::uint8_t> input_, output_;
    std::vector<std::uint8_t> screen_;
    std::uint64_t output_begin_{0}, output_end_{0}, generation_{0};
    VmResult boot_result_;
    std::atomic<bool> quitting_{false};
};
} // namespace domvirt::testing
