#include "archvirt.h"
#include "machine/runtime.hpp"
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

namespace {
struct Handle {
    domvirt::machine::Runtime runtime;
    std::string error;
    ArchVirtSerial serial{};
    void* user{};
    bool initialized{false};
    int check(domvirt::VmResult result) { error = result.message; return result ? 0 : -1; }
};
const char* environment(const char* primary, const char* alias) {
    if (const auto* value = std::getenv(primary); value && *value) return value;
    if (const auto* value = std::getenv(alias); value && *value) return value;
    return nullptr;
}
std::string text(const char* value) { return value ? value : ""; }
}
extern "C" {
void* archvirt_create() { try { return new Handle; } catch (...) { return nullptr; } }
int archvirt_set_serial(void* vm, ArchVirtSerial callback, void* user) {
    if (!vm) return -1;
    auto& h = *static_cast<Handle*>(vm);
    if (h.initialized) { h.error = "Set serial callback before initialization"; return -1; }
    h.serial = callback; h.user = user; return 0;
}
int archvirt_initialize(void* vm, const ArchVirtConfig* config) {
    if (!vm) return -1;
    auto& h = *static_cast<Handle*>(vm);
    try {
        if (!config || config->struct_size != sizeof(*config) || (config->flags & ~3u)) {
            h.error = "Invalid ArchVirtConfig"; return -1;
        }
        domvirt::machine::BootOptions options;
        if (config->machine && *config->machine) options.machine = config->machine;
        if (config->backend && *config->backend) options.backend = config->backend;
        options.archml_path = text(config->archml_path);
        options.kernel_path = text(config->kernel_path); options.image_path = text(config->image_path);
        options.firmware_path = text(config->firmware_path);
        options.cmdline = text(config->cmdline);
        options.memory_mb = config->memory_mb; options.instruction_limit = config->instruction_limit;
        options.bundled_domix = config->flags & ARCHVIRT_DOMIX; options.domios = config->flags & ARCHVIRT_DOMIOS;
        const auto result = h.runtime.initialize(options, [&h](bool write, std::uint8_t value) -> std::uint8_t {
            if (h.serial) return h.serial(h.user, write, value);
            if (write) { std::putchar(value); std::fflush(stdout); return 0; }
            pollfd input{STDIN_FILENO, POLLIN, 0};
            return poll(&input, 1, 1) > 0 && (input.revents & POLLIN) && read(STDIN_FILENO, &value, 1) == 1 ? value : 0;
        });
        if (result) h.initialized = true;
        return h.check(result);
    } catch (const std::exception& error) { h.error = error.what(); return -1; }
    catch (...) { h.error = "Unexpected initialization failure"; return -1; }
}
int archvirt_start(void* vm) {
    if (!vm) return -1;
    auto& h = *static_cast<Handle*>(vm);
    try { return h.check(h.runtime.start()); }
    catch (...) { h.error = "Could not start VM"; return -1; }
}
int archvirt_wait(void* vm) {
    if (!vm) return -1;
    auto& h = *static_cast<Handle*>(vm);
    try { h.runtime.join(); return h.check(h.runtime.result()); }
    catch (...) { h.error = "Could not join VM"; return -1; }
}
void archvirt_stop(void* vm) { if (vm) static_cast<Handle*>(vm)->runtime.stop(); }
void archvirt_destroy(void* vm) { delete static_cast<Handle*>(vm); }
const char* archvirt_last_error(void* vm) { return vm ? static_cast<Handle*>(vm)->error.c_str() : "Null VM handle"; }
const char* archvirt_version() { return DOMVIRT_VERSION; }

void* domvirt_create() { return archvirt_create(); }
int domvirt_initialize(void* vm, const char* kernel, const char* initrd, const char* cmdline, uint64_t memory) {
    if (!kernel || !*kernel) kernel = environment("DOMVIRT_KERNEL", "DOMVIRT_KERNEL_PATH");
    if (!initrd || !*initrd) initrd = environment("DOMVIRT_OS", "DOMVIRT_OS_PATH");
    if (!initrd || !*initrd) initrd = environment("DOMVIRT_INITRD", "DOMVIRT_INITRD_PATH");
    ArchVirtConfig config{};
    config.struct_size = sizeof(config); config.machine = environment("ARCHVIRT_MACHINE", "DOMVIRT_MACHINE");
    config.kernel_path = kernel; config.image_path = initrd;
    config.cmdline = cmdline;
    config.firmware_path = environment("DOMVIRT_BIOS", "DOMVIRT_BIOS_PATH");
    config.memory_mb = memory; config.flags = ARCHVIRT_DOMIOS | ((kernel && *kernel) ? 0 : ARCHVIRT_DOMIX);
    return archvirt_initialize(vm, &config);
}
int domvirt_run(void* vm) { return archvirt_start(vm); }
void domvirt_stop(void* vm) { archvirt_stop(vm); }
void domvirt_destroy(void* vm) { archvirt_destroy(vm); }
const char* domvirt_version() { return archvirt_version(); }
}
