#pragma once
#include "domvirt_types.hpp"
#include <functional>
#include <memory>
#include <span>
#include <map>

namespace domvirt::machine {

struct BootOptions {
    std::string machine{"x86_64"};
    std::string archml_path;
    std::string backend{"auto"};
    std::string kernel_path, image_path, firmware_path, cmdline;
    std::string boot_mode{"direct"}, disk_path;
    std::uint64_t memory_mb{64};
    std::uint64_t instruction_limit{0};
    bool bundled_domix{true}, domios{true};
};

class Runtime {
public:
    using Serial = std::function<std::uint8_t(bool write, std::uint8_t value)>;
    using Console = std::function<void(std::span<const std::uint8_t>)>;
    Runtime();
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    VmResult initialize(const BootOptions& options, Serial serial = {}, Console console = {});
    VmResult start(bool paused = false);
    VmResult pause();
    VmResult resume();
    VmResult step(std::uint64_t instructions = 1);
    // Inspection is only permitted while the CPU is quiescent.
    VmResult read_memory(std::uint64_t address, std::size_t size, std::vector<std::uint8_t>& data) const;
    VmResult write_memory(std::uint64_t address, std::span<const std::uint8_t> data);
    VmResult registers(std::map<std::string, std::uint64_t>& values) const;
    std::string state() const;
    void stop();
    void join();
    bool running() const;
    VmResult result() const;
    std::uint64_t instruction_count() const;
    std::string backend() const;
    std::string machine_name() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace domvirt::machine
