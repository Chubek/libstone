#include <iostream>
#include <chrono>
#include "hypervisor/kvm_driver.hpp"
#include "hypervisor/kvm_vm.hpp"
#include "hypervisor/vcpu.hpp"

using namespace domvirt::hypervisor;
using Clock = std::chrono::high_resolution_clock;

int main() {
    KvmDriver driver;
    auto result = driver.initialize();
    if (!result) {
        std::cerr << "Skipping: KVM not available\n";
        return 0;
    }

    KvmVm vm(driver);
    result = vm.create();
    if (!result) {
        std::cerr << "Skipping: VM create failed\n";
        return 0;
    }

    result = vm.memory().allocate_ram(1024 * 1024);
    if (!result) {
        std::cerr << "Skipping: memory alloc failed\n";
        return 0;
    }
    vm.memory().register_all_slots(vm.fd());

    VcpuConfig cfg{};
    cfg.rip = 0x1000;
    cfg.rsp = 0x8000;

    VcpuThread vcpu(vm, cfg);
    result = vcpu.initialize();
    if (!result) {
        std::cerr << "Skipping: VCPU init failed\n";
        return 0;
    }

    auto start = Clock::now();
    vcpu.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    vcpu.stop();
    vcpu.join();
    auto end = Clock::now();

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "VCPU loop benchmark: " << elapsed.count() << " us\n";
    return 0;
}
