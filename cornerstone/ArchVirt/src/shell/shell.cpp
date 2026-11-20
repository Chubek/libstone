#include <iostream>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <readline/readline.h>
#include <readline/history.h>
#include "hypervisor/kvm_driver.hpp"
#include "machine/runtime.hpp"
#include "machine/profile.hpp"
#include "bindlet_bridge/bridge.hpp"

using namespace domvirt;

static std::atomic<bool> shell_running{true};

[[maybe_unused]] static void print_prompt() {
    std::cout << "domvirt> " << std::flush;
}

static void handle_version() {
    std::cout << "domvirt-shell " << DOMVIRT_VERSION << "\n";
}

static void handle_help() {
    std::cout << "Commands:\n"
              << "  version          Show version\n"
              << "  help             Show this help\n"
              << "  kvm-info         Show KVM driver info\n"
              << "  caps             List KVM capabilities\n"
              << "  machines         List supported machines\n"
              << "  vm-create [ISA]  Prepare a bundled VM (default: x86_64)\n"
              << "  vm-status        Show VM status\n"
              << "  memory STATS     Show memory stats\n"
              << "  bridge-init      Initialize Python bridge\n"
              << "  bridge-exec MOD FUNC ARGS  Execute Python\n"
              << "  exit, quit       Exit shell\n";
}

static void handle_kvm_info(hypervisor::KvmDriver* driver) {
    if (!driver || !driver->is_initialized()) {
        std::cout << "KVM driver not initialized\n";
        return;
    }
    std::cout << "KVM API version: " << driver->api_version() << "\n"
              << "VCPU mmap size: " << driver->vcpu_mmap_size() << "\n";
}

static void handle_caps(hypervisor::KvmDriver* driver) {
    if (!driver || !driver->is_initialized()) {
        std::cout << "KVM driver not initialized\n";
        return;
    }
    auto caps = driver->query_capabilities();
    for (auto& cap : caps) {
        std::cout << "  " << cap.name << ": "
                  << (cap.supported ? "YES" : "no") << "\n";
    }
}

static void handle_bridge_exec(bindlet_bridge::Bridge* bridge,
                               const std::string& mod,
                               const std::string& func,
                               const std::string& args) {
    if (!bridge) {
        std::cout << "Bridge not initialized\n";
        return;
    }
    std::string result;
    auto r = bridge->execute_python(mod, func, args, result);
    if (r) {
        std::cout << "Result: " << result << "\n";
    } else {
        std::cout << "Error: " << r.message << "\n";
    }
}

int main() {
    std::cout << "domvirt-shell " << DOMVIRT_VERSION << "\n"
              << "Type 'help' for commands, 'exit' to quit.\n\n";

    hypervisor::KvmDriver driver;
    std::unique_ptr<machine::Runtime> vm;
    bindlet_bridge::Bridge bridge;

    while (shell_running) {
        char* line = readline("domvirt> ");
        if (!line) break;

        std::string input(line);
        free(line);

        if (input.empty()) continue;
        add_history(input.c_str());

        std::istringstream iss(input);
        std::string cmd;
        iss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            shell_running = false;
        } else if (cmd == "version") {
            handle_version();
        } else if (cmd == "help") {
            handle_help();
        } else if (cmd == "kvm-info") {
            handle_kvm_info(&driver);
        } else if (cmd == "caps") {
            handle_caps(&driver);
        } else if (cmd == "kvm-init") {
            auto result = driver.initialize();
            std::cout << (result ? "OK" : "FAIL") << ": " << result.message << "\n";
        } else if (cmd == "vm-create") {
            machine::BootOptions options;
            iss >> options.machine;
            vm = std::make_unique<machine::Runtime>();
            auto result = vm->initialize(options);
            std::cout << (result ? "OK" : "FAIL") << ": " << result.message << "\n";
        } else if (cmd == "machines") {
            for (const auto* name : {"x86_64", "aarch64", "riscv64"}) std::cout << name << '\n';
        } else if (cmd == "vm-status") {
            if (!vm) {
                std::cout << "No VM created\n";
            } else {
                std::cout << vm->machine_name() << " / " << vm->backend() << "\n";
            }
        } else if (cmd == "memory") {
            std::string sub;
            iss >> sub;
            if (vm) {
                std::cout << "Bundled VM: 64 MiB RAM\n";
            } else {
                std::cout << "No VM\n";
            }
        } else if (cmd == "bridge-init") {
            auto result = bridge.initialize();
            std::cout << (result ? "OK" : "FAIL") << ": " << result.message << "\n";
        } else if (cmd == "bridge-exec") {
            std::string mod, func, args;
            iss >> mod >> func;
            std::getline(iss, args);
            if (!args.empty() && args[0] == ' ') args.erase(0, 1);
            handle_bridge_exec(&bridge, mod, func, args);
        } else {
            std::cout << "Unknown command: " << cmd << " (type 'help')\n";
        }
    }

    std::cout << "\nGoodbye.\n";
    return 0;
}
