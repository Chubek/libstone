#include "runtime.hpp"
#include "profile.hpp"
#include "pc_devices.hpp"
#include "archvirt_boot.h"
#include "boot/embedded_images.hpp"
#include "boot/data_path.hpp"
#include "boot/loader.hpp"
#include "virtual/virtual_cpu.hpp"
#include "virtual/aarch64_cpu.hpp"
#include "virtual/riscv64_cpu.hpp"
#include "firmware.hpp"
#include "dit.hpp"
#include "lua_runtime.hpp"
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <filesystem>
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <tuple>

#if defined(__x86_64__)
#include "hypervisor/kvm_driver.hpp"
#include "hypervisor/kvm_vm.hpp"
#include "hypervisor/vcpu.hpp"
#endif

namespace domvirt::machine {
namespace {
std::vector<std::uint8_t> read_image(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() < 0 || in.tellg() > 64 * 1024 * 1024)
        throw std::runtime_error("Cannot read image (maximum 64 MiB): " + path);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Truncated image read: " + path);
    return bytes;
}
std::uint64_t le(std::span<const std::uint8_t> data, std::size_t at, unsigned size) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(data[at + i]) << (8 * i);
    return value;
}
void validate_domix(std::span<const std::uint8_t> image, unsigned architecture) {
    if (image.size() < 40 || std::memcmp(image.data(), "DOMIXIMG", 8) || le(image, 8, 4) != 1 ||
        le(image, 12, 4) != architecture) throw std::runtime_error("Invalid Domix image or architecture mismatch");
    std::size_t at = 40;
    auto advance = [&](std::uint64_t size) {
        if (size > image.size() - at) throw std::runtime_error("Truncated Domix image");
        at += size;
    };
    advance(le(image, 16, 8)); advance(le(image, 24, 8));
    const auto count = le(image, 32, 8);
    if (!count || count > 128) throw std::runtime_error("Domix image file count exceeds guest capacity");
    std::set<std::string> paths;
    for (std::uint64_t i = 0; i < count; ++i) {
        auto start = at; advance(4); const auto size = le(image, start, 4);
        if (!size || size > 126) throw std::runtime_error("Invalid Domix image path length");
        start = at; advance(size);
        std::string path(reinterpret_cast<const char*>(image.data() + start), size);
        if (path[0] == '/' || path.find('\0') != std::string::npos || !paths.insert(path).second)
            throw std::runtime_error("Invalid or duplicate Domix image path");
        for (const auto& part : std::filesystem::path(path))
            if (part == "." || part == ".." || part.empty()) throw std::runtime_error("Invalid Domix image path component");
        start = at; advance(12);
        const auto mode = le(image, start, 4), bytes = le(image, start + 4, 8);
        if ((mode & ~0040777u) || ((mode & 0040000) && (bytes || path == "init")))
            throw std::runtime_error("Invalid Domix file mode or directory payload");
        advance(bytes);
    }
    if (at != image.size() || !paths.contains("init")) throw std::runtime_error("Domix image requires /init and exact section bounds");
}
}

struct Runtime::Impl {
    BootOptions options;
    Profile profile;
    std::string backend_name{"software"};
    hypervisor::MemoryManager ram;
    hypervisor::MemoryManager* memory{&ram};
#if defined(__x86_64__)
    hypervisor::KvmDriver driver;
    std::unique_ptr<hypervisor::KvmVm> vm;
    std::unique_ptr<hypervisor::VcpuThread> kvm;
#endif
    std::unique_ptr<virtualization::VirtualCpu> x86;
    std::unique_ptr<virtualization::Aarch64Cpu> arm;
    std::unique_ptr<virtualization::Riscv64Cpu> riscv;
    std::unique_ptr<PcDevices> pc;
    Serial serial;
    Console console;
    std::atomic<bool> active{false}, stopping{false};
    enum class State { ready, running, paused, halted, stopped, failed };
    std::atomic<State> state{State::ready};
    std::atomic<bool> pause_requested{false};
    mutable std::mutex execution_mutex;
    std::condition_variable execution_changed;
    std::atomic<std::uint64_t> count{0};
    mutable std::mutex mutex;
    VmResult status;
    std::jthread worker;
    bool started{false};

    Impl(const BootOptions& opts, Serial io, Console display)
        : options(opts), profile(Profile::load(opts.machine, opts.archml_path)),
          serial(std::move(io)), console(std::move(display)) {}
    ~Impl() {
        { std::lock_guard lock(execution_mutex); stopping.store(true); }
        execution_changed.notify_all();
        if (worker.joinable()) worker.join();
    }
    void publish_console() {
        if (console) console(memory->get_span(profile.console_base,
            profile.console_columns * profile.console_rows * 2));
    }
    VmResult prepare() {
        if (options.boot_mode != "direct" && options.boot_mode != "bios")
            return {VmErrorCode::BOOT_LOAD_FAILED, "boot_mode must be direct or bios"};
        if (options.boot_mode == "bios" && (profile.architecture != Architecture::x86_64 ||
            options.backend == "software" || options.instruction_limit || options.disk_path.empty() ||
            !options.kernel_path.empty() || !options.image_path.empty() || !options.cmdline.empty()))
            return {VmErrorCode::BOOT_LOAD_FAILED, "BIOS requires x86_64 KVM, a disk, and no direct kernel/image/cmdline or instruction limit"};
        if (options.boot_mode == "direct" && !options.disk_path.empty())
            return {VmErrorCode::BOOT_LOAD_FAILED, "disk requires boot_mode=bios"};
        if (options.backend != "auto" && options.backend != "software" && options.backend != "kvm")
            return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Backend must be auto, software, or kvm"};
        if (options.memory_mb < 40 || options.memory_mb > 1024)
            return {VmErrorCode::MEMORY_ALLOC_FAILED, "DomIOS machines require 40..1024 MiB RAM"};
        if (options.backend == "kvm" && !profile.native_kvm())
            return {VmErrorCode::VCPU_CREATE_FAILED, "No native KVM adapter for selected machine on this host; use software"};
        if (options.instruction_limit && options.backend == "kvm")
            return {VmErrorCode::VCPU_CREATE_FAILED, "Instruction limits require the software backend"};
#if defined(__x86_64__)
        if (options.backend != "software" && profile.native_kvm() && !options.instruction_limit) {
            auto result = driver.initialize();
            if (result) {
                vm = std::make_unique<hypervisor::KvmVm>(driver);
                result = vm->create();
                // The bundled ABI polls its console and ends with HLT. Leave
                // interrupts in userspace so KVM reports HLT as a VM exit.
            }
            if (!result && options.backend == "kvm") return result;
            if (result) { memory = &vm->memory(); backend_name = "kvm"; }
            else vm.reset();
        }
#endif
        const std::uint64_t bytes = options.memory_mb * 1024 * 1024;
        if (options.boot_mode == "bios") return prepare_bios(bytes);
        auto result = memory->allocate_ram(bytes, 0, false);
        if (!result) return result;
        auto* guest = memory->translate(0);
        boot::BootLoader loader;
        ArchVirtBootInfo info{};
        info.magic = ARCHVIRT_BOOT_MAGIC; info.version = 1;
        info.architecture = static_cast<std::uint16_t>(profile.architecture);
        info.memory_size = bytes;
        info.console_base = profile.console_base;
        info.console_columns = profile.console_columns; info.console_rows = profile.console_rows;
        info.serial_base = profile.serial_base;

        if (options.domios) {
            std::vector<std::uint8_t> owned;
            auto embedded = boot::embedded_domios_image();
            std::span<const std::uint8_t> image;
            if (options.firmware_path.empty() && embedded) image = {embedded.data, embedded.size};
            else {
                owned = read_image(options.firmware_path.empty() ? data_path("domios/domios.img", DOMVIRT_DEFAULT_DOMIOS_IMAGE) : options.firmware_path);
                image = owned;
            }
            std::vector<domios::FirmwarePart> sections; std::string error;
            if (!domios::unpack_firmware(image, sections, error)) return {VmErrorCode::BOOT_LOAD_FAILED, error};
            domios::LiosRuntime firmware;
            domios::EventQueue events(16);
            if (!firmware.initialize(domios::Config{}, events, error))
                return {VmErrorCode::BOOT_LOAD_FAILED, error};
            for (const auto& section : sections) {
                if (section.kind != domios::FirmwareSection::lua) continue;
                if (!firmware.load_embedded_program("boot", {section.bytes.begin(), section.bytes.end()}))
                    return {VmErrorCode::BOOT_LOAD_FAILED, "Invalid DomIOS boot program"};
                firmware.run_program("boot", {profile.name}, events, &error);
                if (!error.empty()) return {VmErrorCode::BOOT_LOAD_FAILED, "DomIOS: " + error};
            }
            info.firmware_base = profile.firmware_base; info.firmware_size = image.size(); info.flags = 1;
            result = loader.load_blob(image.data(), image.size(), guest, bytes, info.firmware_base, info.firmware_size);
            if (!result) return result;
            auto dit = domios::make_default_dit();
            dit.header.phase = static_cast<std::uint8_t>(domios::Phase::shell);
            dit.framebuffer.base = profile.console_base;
            dit.framebuffer.width = profile.console_columns; dit.framebuffer.height = profile.console_rows;
            dit.framebuffer.pitch = profile.console_columns * 2; dit.framebuffer.bpp = 16;
            dit.framebuffer.format = static_cast<std::uint8_t>(domios::DitFormat::vga_text);
            dit.progress.percent = 100;
            const auto published = domios::serialize_dit(dit);
            std::memcpy(guest + domios::kDitDefaultBase, published.bytes.data(), published.bytes.size());
        }

        std::vector<std::uint8_t> owned_image, owned_kernel;
        std::span<const std::uint8_t> image, kernel;
        if (!options.image_path.empty()) { owned_image = read_image(options.image_path); image = owned_image; }
        else if (options.bundled_domix) {
            auto embedded = boot::embedded_domix_image(info.architecture);
            if (embedded) image = {embedded.data, embedded.size};
            else {
                const auto relative = std::string("domix/") + profile.name + "/domix.img";
                const auto fallback = std::string(DOMVIRT_DOMIX_DIR) + "/" + profile.name + "/domix.img";
                owned_image = read_image(data_path(relative.c_str(), fallback.c_str())); image = owned_image;
            }
        }
        if (!image.empty()) {
            if (image.size() > profile.firmware_base - profile.initrd_base)
                return {VmErrorCode::BOOT_LOAD_FAILED, "Guest image overlaps firmware region"};
            info.image_base = profile.initrd_base; info.image_size = image.size();
            if (options.bundled_domix) {
                validate_domix(image, info.architecture);
                kernel = image.subspan(40, le(image, 16, 8));
            }
            result = loader.load_blob(image.data(), image.size(), guest, bytes, info.image_base, info.image_size);
            if (!result) return result;
        }
        if (!options.kernel_path.empty()) { owned_kernel = read_image(options.kernel_path); kernel = owned_kernel; }
        if (kernel.empty()) return {VmErrorCode::BOOT_LOAD_FAILED, "No kernel supplied"};
        std::uint64_t entry = 0;
        result = loader.load_elf(kernel, guest, bytes, entry, info.architecture, profile.kernel_base, 0x600000);
        if (!result) return result;
        if (options.cmdline.size() > 2048)
            return {VmErrorCode::BOOT_LOAD_FAILED, "Command line exceeds 2048 bytes"};
        info.cmdline_base = 0x11000; info.cmdline_size = options.cmdline.size();
        std::memcpy(guest + info.cmdline_base, options.cmdline.c_str(), options.cmdline.size() + 1);
        static_assert(sizeof(info) == 96);
        std::memcpy(guest + profile.boot_info, &info, sizeof(info));
#if defined(__x86_64__)
        if (backend_name == "kvm") {
            result = memory->setup_identity_paging();
            if (result) result = memory->register_all_slots(vm->fd());
            if (!result) return result;
            hypervisor::VcpuConfig cfg{};
            cfg.rip = entry; cfg.rsp = profile.stack_top; cfg.boot_argument = profile.boot_info;
            cfg.cr0 = 0x80000011; cfg.cr3 = 0x9000; cfg.cr4 = 0x20; cfg.efer = 0x500;
            kvm = std::make_unique<hypervisor::VcpuThread>(*vm, cfg);
            kvm->set_exit_handler([this](struct kvm_run* run) -> VmResult {
                if (run->exit_reason != KVM_EXIT_IO || run->io.port != profile.serial_base || run->io.size != 1)
                    return {VmErrorCode::DEVICE_INIT_FAILED, "Guest accessed an unsupported device"};
                auto* data = reinterpret_cast<std::uint8_t*>(run) + run->io.data_offset;
                for (unsigned i = 0; i < run->io.count; ++i) {
                    const bool write = run->io.direction == KVM_EXIT_IO_OUT;
                    const auto value = serial ? serial(write, data[i]) : 0;
                    if (!write) data[i] = value;
                }
                return {};
            });
            return kvm->initialize();
        }
#endif
        if (profile.architecture == Architecture::aarch64) {
            arm = std::make_unique<virtualization::Aarch64Cpu>(*memory, profile, entry, profile.boot_info, serial);
        } else if (profile.architecture == Architecture::riscv64) {
            riscv = std::make_unique<virtualization::Riscv64Cpu>(*memory, profile, entry, profile.boot_info, serial);
        } else {
            virtualization::VirtualCpuConfig cfg{};
            cfg.rip = entry; cfg.rsp = profile.stack_top; cfg.boot_argument = profile.boot_info;
            x86 = std::make_unique<virtualization::VirtualCpu>(*memory, cfg);
            x86->set_io_handler([this](std::uint16_t port, bool write, std::uint8_t size, std::uint8_t value) {
                return port == profile.serial_base && size == 1 && serial ? serial(write, value) : std::uint8_t{0};
            });
            return x86->initialize();
        }
        return {};
    }
    VmResult prepare_bios(std::uint64_t bytes) {
#if defined(__x86_64__)
        if (backend_name != "kvm") return {VmErrorCode::VCPU_CREATE_FAILED, "BIOS requires native KVM"};
        if (!driver.check_extension(KVM_CAP_READONLY_MEM))
            return {VmErrorCode::VCPU_CREATE_FAILED, "BIOS requires KVM read-only memory slots"};
        auto platform = vm->set_tss_addr(0xfffbd000); if (!platform) return platform;
        platform = vm->set_identity_map_addr(0xfffbc000); if (!platform) return platform;
        auto disk = read_image(options.disk_path);
        if (disk.empty() || disk.size() % 512)
            return {VmErrorCode::BOOT_LOAD_FAILED, "BIOS disk must contain whole 512-byte sectors (maximum 64 MiB)"};
        auto rom = read_image(options.firmware_path.empty() ?
            data_path("bios/archvirt-bios.bin", ARCHVIRT_PC_BIOS) : options.firmware_path);
        if (rom.size() != 65536 || rom[0xfff0] != 0xea)
            return {VmErrorCode::BOOT_LOAD_FAILED, "B1 BIOS ROM must be 64 KiB with its reset far jump"};
        auto put = [&](std::size_t at, std::uint64_t value, unsigned size) {
            for (unsigned i = 0; i < size; ++i) rom[at+i] = value >> (8*i);
        };
        put(0xff00, bytes, 4); put(0xff04, disk.size()/512, 4);
        put(0xff10+2*24+8, bytes-0x100000, 8);
        rom.back() = 0; unsigned sum = 0; for (auto b : rom) sum += b;
        rom.back() = static_cast<std::uint8_t>(0-sum);
        for (auto [base, size, ro] : {std::tuple<std::uint64_t,std::uint64_t,bool>{0,0xf0000,false},
                {0xf0000,0x10000,true}, {0x100000,bytes-0x100000,false}, {0xffff0000,0x10000,true}}) {
            auto result = memory->allocate_ram(size, base, false, ro); if (!result) return result;
            if (ro) std::memcpy(memory->translate(base), rom.data(), rom.size());
        }
        auto result = memory->register_all_slots(vm->fd()); if (!result) return result;
        pc = std::make_unique<PcDevices>(std::move(disk), serial);
        hypervisor::VcpuConfig cfg{}; cfg.reset_real_mode = true;
        kvm = std::make_unique<hypervisor::VcpuThread>(*vm, cfg);
        kvm->set_exit_handler([this](struct kvm_run* run) -> VmResult {
            if (run->exit_reason != KVM_EXIT_IO)
                return {VmErrorCode::DEVICE_INIT_FAILED, "Unsupported BIOS MMIO/hypercall (ROM is read-only)"};
            auto* data = reinterpret_cast<std::uint8_t*>(run) + run->io.data_offset;
            const auto size = run->io.size;
            if (size != 1 && size != 2 && size != 4)
                return {VmErrorCode::DEVICE_INIT_FAILED, "Invalid BIOS I/O width"};
            for (unsigned i = 0; i < run->io.count; ++i) {
                std::uint32_t value = 0; std::memcpy(&value, data+i*size, size);
                if (!pc->io(run->io.port, run->io.direction == KVM_EXIT_IO_OUT, size, value))
                    return {VmErrorCode::DEVICE_INIT_FAILED, "Unsupported BIOS port: " + std::to_string(run->io.port)};
                if (run->io.direction == KVM_EXIT_IO_IN) std::memcpy(data+i*size, &value, size);
            }
            return {};
        });
        return kvm->initialize();
#else
        return {VmErrorCode::VCPU_CREATE_FAILED, "BIOS requires native x86_64 KVM"};
#endif
    }
    VmResult single_step() {
        if (options.instruction_limit && count.load() >= options.instruction_limit)
            return {VmErrorCode::TIMEOUT, "Guest instruction limit reached"};
        auto result = riscv ? riscv->step() : arm ? arm->step() : x86->step();
        if (result) ++count;
        return result;
    }
    bool halted() const { return riscv ? riscv->halted() : arm ? arm->halted() : x86 && x86->halted(); }
    void pause_point() {
        publish_console();
        std::unique_lock lock(execution_mutex);
        state.store(State::paused);
        execution_changed.notify_all();
        execution_changed.wait(lock, [&] { return stopping.load() || !pause_requested.load(); });
        if (!stopping.load()) state.store(State::running);
        execution_changed.notify_all();
    }
    void execute() {
        VmResult last{};
        bool guest_halted = false;
        try {
#if defined(__x86_64__)
            if (kvm) {
                while (!stopping.load()) {
                    if (pause_requested.load()) pause_point();
                    if (stopping.load()) break;
                    last = kvm->run();
                    while (last && !stopping.load() && !pause_requested.load() && kvm->is_running()) {
                        publish_console();
                        std::this_thread::sleep_for(std::chrono::milliseconds(4));
                    }
                    kvm->stop(); kvm->join();
                    guest_halted = kvm->halted();
                    if (last) last = kvm->result();
                    if (!last || guest_halted) break;
                }
            } else
#endif
            {
                while (!stopping.load()) {
                    if (pause_requested.load()) pause_point();
                    if (stopping.load()) break;
                    last = single_step();
                    if (!last) break;
                    if (halted()) { guest_halted = true; break; }
                    if ((count.load() & 4095) == 0) { publish_console(); std::this_thread::yield(); }
                }
            }
            publish_console();
        } catch (const std::exception& error) { last = {VmErrorCode::VIRTUAL_BACKEND_ERROR, error.what()}; }
#if defined(__x86_64__)
        // A failing callback must also leave the native CPU quiescent.
        if (kvm) { kvm->stop(); kvm->join(); }
#endif
        std::lock_guard execution_lock(execution_mutex);
        {
            std::lock_guard lock(mutex);
            if (status) status = std::move(last);
            state.store(!status ? State::failed : guest_halted || halted() ? State::halted : State::stopped);
        }
        active.store(false);
        execution_changed.notify_all();
    }
};

Runtime::Runtime() = default;
Runtime::~Runtime() { stop(); join(); }
VmResult Runtime::initialize(const BootOptions& options, Serial serial, Console console) {
    if (running()) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Cannot reinitialize a running VM"};
    join(); impl_.reset();
    try {
        auto instance = std::make_unique<Impl>(options, std::move(serial), std::move(console));
        auto result = instance->prepare();
        if (!result) return result;
        impl_ = std::move(instance);
        return {};
    } catch (const std::exception& error) { return {VmErrorCode::BOOT_LOAD_FAILED, error.what()}; }
}
VmResult Runtime::start(bool paused) {
    if (!impl_ || impl_->started) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM must be initialized before each run"};
    impl_->started = true; impl_->stopping.store(false); impl_->active.store(true);
    impl_->pause_requested.store(paused);
    impl_->state.store(Impl::State::running);
    try { impl_->worker = std::jthread([this] { impl_->execute(); }); }
    catch (const std::exception& error) {
        std::lock_guard lock(impl_->mutex);
        impl_->status = {VmErrorCode::VIRTUAL_BACKEND_ERROR, error.what()};
        impl_->state.store(Impl::State::failed); impl_->active.store(false);
        return impl_->status;
    }
    return paused ? pause() : VmResult{};
}
VmResult Runtime::pause() {
    if (!impl_ || !impl_->active.load()) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not running"};
    std::unique_lock lock(impl_->execution_mutex);
    impl_->pause_requested.store(true);
    impl_->execution_changed.wait(lock, [&] { return !impl_->active.load() || impl_->state.load() == Impl::State::paused; });
    return {};
}
VmResult Runtime::resume() {
    if (!impl_) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not initialized"};
    std::unique_lock lock(impl_->execution_mutex);
    if (impl_->state.load() != Impl::State::paused || impl_->stopping.load())
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not paused"};
    impl_->pause_requested.store(false);
    impl_->execution_changed.notify_all();
    impl_->execution_changed.wait(lock, [&] { return impl_->state.load() != Impl::State::paused; });
    return {};
}
VmResult Runtime::step(std::uint64_t instructions) {
    if (!impl_) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not initialized"};
    std::unique_lock lock(impl_->execution_mutex);
    if (impl_->backend_name != "software") return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Single stepping requires software execution"};
    if (impl_->state.load() != Impl::State::paused || impl_->stopping.load())
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Pause the VM before stepping"};
    if (!instructions || instructions > 10000) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Step count must be 1..10000"};
    VmResult result;
    try {
        for (std::uint64_t i = 0; i < instructions; ++i) {
            result = impl_->single_step();
            if (!result || impl_->halted()) break;
        }
        impl_->publish_console();
    } catch (const std::exception& error) { result = {VmErrorCode::VIRTUAL_BACKEND_ERROR, error.what()}; }
    if (!result || impl_->halted()) {
        { std::lock_guard result_lock(impl_->mutex); impl_->status = result; }
        impl_->stopping.store(true);
        impl_->execution_changed.notify_all();
        impl_->execution_changed.wait(lock, [&] { return !impl_->active.load(); });
    }
    return result;
}
VmResult Runtime::read_memory(std::uint64_t address, std::size_t size, std::vector<std::uint8_t>& data) const {
    if (!impl_) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not initialized"};
    std::lock_guard lock(impl_->execution_mutex);
    if (impl_->state.load() == Impl::State::running) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Pause the VM before inspecting memory"};
    if (!size || size > 65536) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Memory size must be 1..65536"};
    const auto bytes = impl_->memory->get_span(address, size);
    if (bytes.size() != size) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Memory range is outside guest RAM"};
    data.assign(bytes.begin(), bytes.end()); return {};
}
VmResult Runtime::write_memory(std::uint64_t address, std::span<const std::uint8_t> data) {
    if (!impl_) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not initialized"};
    std::lock_guard lock(impl_->execution_mutex);
    if (impl_->state.load() != Impl::State::paused) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Pause the VM before changing memory"};
    if (data.empty() || data.size() > 65536) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Memory size must be 1..65536"};
    if (impl_->options.boot_mode == "bios" && ((address >= 0xf0000 && address < 0x100000) || address >= 0xffff0000))
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "BIOS ROM is read-only"};
    const auto bytes = impl_->memory->get_span(address, data.size());
    if (bytes.size() != data.size()) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Memory range is outside guest RAM"};
    std::copy(data.begin(), data.end(), bytes.begin()); return {};
}
VmResult Runtime::registers(std::map<std::string, std::uint64_t>& values) const {
    if (!impl_) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not initialized"};
    std::lock_guard lock(impl_->execution_mutex);
    if (impl_->state.load() == Impl::State::running) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Pause the VM before inspecting registers"};
    values.clear();
    if (impl_->riscv) {
        const auto& cpu = impl_->riscv->state();
        values["pc"] = cpu.pc; values["sp"] = cpu.x[2];
        for (unsigned i = 0; i < cpu.x.size(); ++i) values["x" + std::to_string(i)] = cpu.x[i];
    } else if (impl_->arm) {
        const auto& cpu = impl_->arm->state();
        values["pc"] = cpu.pc; values["sp"] = cpu.sp; values["nzcv"] = cpu.nzcv;
        for (unsigned i = 0; i < cpu.x.size(); ++i) values["x" + std::to_string(i)] = cpu.x[i];
    } else {
        VcpuRegisters cpu{};
#if defined(__x86_64__)
        if (impl_->kvm) { auto result = impl_->kvm->get_regs(cpu); if (!result) return result; }
        else
#endif
        cpu = impl_->x86->registers();
        values = {{"pc", cpu.rip}, {"sp", cpu.rsp}, {"rip", cpu.rip}, {"rsp", cpu.rsp},
            {"rflags", cpu.rflags}, {"rax", cpu.rax}, {"rbx", cpu.rbx}, {"rcx", cpu.rcx}, {"rdx", cpu.rdx},
            {"rsi", cpu.rsi}, {"rdi", cpu.rdi}, {"rbp", cpu.rbp}, {"r8", cpu.r8}, {"r9", cpu.r9},
             {"r10", cpu.r10}, {"r11", cpu.r11}, {"r12", cpu.r12}, {"r13", cpu.r13}, {"r14", cpu.r14}, {"r15", cpu.r15}};
#if defined(__x86_64__)
        if (impl_->kvm) {
            kvm_sregs s{}; auto result = impl_->kvm->get_sregs(s); if (!result) return result;
            values["cs"] = s.cs.selector; values["cs_base"] = s.cs.base;
            values["cr0"] = s.cr0; values["cr3"] = s.cr3; values["efer"] = s.efer;
        }
#endif
    }
    return {};
}
std::string Runtime::state() const {
    if (!impl_) return "empty";
    switch (impl_->state.load()) {
    case Impl::State::ready: return "ready";
    case Impl::State::running: return "running";
    case Impl::State::paused: return "paused";
    case Impl::State::halted: return "halted";
    case Impl::State::stopped: return "stopped";
    case Impl::State::failed: return "failed";
    }
    return "empty";
}
void Runtime::stop() {
    if (!impl_) return;
    { std::lock_guard lock(impl_->execution_mutex); impl_->stopping.store(true); }
    impl_->execution_changed.notify_all();
}
void Runtime::join() { if (impl_ && impl_->worker.joinable()) impl_->worker.join(); }
bool Runtime::running() const { return impl_ && impl_->active.load(); }
VmResult Runtime::result() const {
    if (!impl_) return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "VM is not initialized"};
    std::lock_guard lock(impl_->mutex); return impl_->status;
}
std::uint64_t Runtime::instruction_count() const { return impl_ ? impl_->count.load() : 0; }
std::string Runtime::backend() const { return impl_ ? impl_->backend_name : ""; }
std::string Runtime::machine_name() const { return impl_ ? impl_->profile.name : ""; }
} // namespace domvirt::machine
