#include "vcpu.hpp"
#include "kvm_vm.hpp"
#include "bindlet_bridge/bridge.hpp"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <pthread.h>

namespace domvirt::hypervisor {

VcpuThread::VcpuThread(KvmVm& vm, VcpuConfig config)
    : vm_(vm), config_(config) {}

VcpuThread::~VcpuThread() {
    stop();
    join();
    if (run_) {
        munmap(run_, mmap_size_);
    }
    if (vcpu_fd_ >= 0) close(vcpu_fd_);
}

VmResult VcpuThread::initialize() {
    vcpu_fd_ = ioctl(vm_.fd(), KVM_CREATE_VCPU, config_.vcpu_id);
    if (vcpu_fd_ < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_CREATE_VCPU failed: " + std::string(strerror(errno))};
    }

    mmap_size_ = vm_.driver().vcpu_mmap_size();
    run_ = static_cast<struct kvm_run*>(
        mmap(nullptr, mmap_size_, PROT_READ | PROT_WRITE,
             MAP_SHARED, vcpu_fd_, 0));
    if (run_ == MAP_FAILED) {
        run_ = nullptr;
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "mmap kvm_run failed: " + std::string(strerror(errno))};
    }

    // KVM's default CPUID does not advertise long mode. Install supported
    // features before enabling EFER.LME and paging.
    std::vector<std::uint64_t> cpuid_storage((sizeof(kvm_cpuid2) +
        256 * sizeof(kvm_cpuid_entry2) + 7) / 8);
    auto* cpuid = reinterpret_cast<kvm_cpuid2*>(cpuid_storage.data());
    cpuid->nent = 256;
    if (ioctl(vm_.driver().fd(), KVM_GET_SUPPORTED_CPUID, cpuid) < 0 ||
        ioctl(vcpu_fd_, KVM_SET_CPUID2, cpuid) < 0)
        return {VmErrorCode::VCPU_CREATE_FAILED, "Cannot configure KVM CPUID: " + std::string(strerror(errno))};

    struct kvm_sregs sregs{};
    if (ioctl(vcpu_fd_, KVM_GET_SREGS, &sregs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_GET_SREGS failed: " + std::string(strerror(errno))};
    }

    if (config_.reset_real_mode) {
        // Architectural reset fetch at FFFFFFF0, followed by the ROM's far jump
        // to the low F000 segment. Do not install direct-boot page tables.
        sregs.cr0 = 0x60000010; sregs.cr3 = 0; sregs.cr4 = 0; sregs.efer = 0;
        sregs.cs = {}; sregs.cs.selector = 0xf000; sregs.cs.base = 0xffff0000;
        sregs.cs.limit = 0xffff; sregs.cs.type = 11; sregs.cs.present = 1; sregs.cs.s = 1;
        kvm_segment data{}; data.limit = 0xffff; data.type = 3; data.present = 1; data.s = 1;
        sregs.ds = sregs.es = sregs.fs = sregs.gs = sregs.ss = data;
        sregs.idt.base = 0; sregs.idt.limit = 0x3ff;
        sregs.gdt.base = 0; sregs.gdt.limit = 0xffff;
    } else {
    sregs.cr0 = config_.cr0;
    sregs.cr3 = config_.cr3;
    sregs.cr4 = config_.cr4;
    sregs.efer = config_.efer;

    sregs.cs.base = 0;
    sregs.cs.limit = ~0u;
    sregs.cs.selector = 0x10;
    sregs.cs.type = 11;
    sregs.cs.present = 1;
    sregs.cs.dpl = 0;
    sregs.cs.db = 0;
    sregs.cs.s = 1;
    sregs.cs.l = 1;
    sregs.cs.g = 1;

    sregs.ds.base = sregs.es.base = sregs.fs.base = sregs.gs.base = sregs.ss.base = 0;
    sregs.ds.limit = sregs.es.limit = sregs.fs.limit = sregs.gs.limit = sregs.ss.limit = ~0u;
    sregs.ds.selector = sregs.es.selector = sregs.fs.selector = sregs.gs.selector = 0x18;
    sregs.ss.selector = 0x18;
    sregs.ds.type = sregs.es.type = sregs.fs.type = sregs.gs.type = sregs.ss.type = 3;
    sregs.ds.present = sregs.es.present = sregs.fs.present = sregs.gs.present = sregs.ss.present = 1;
    sregs.ds.dpl = sregs.es.dpl = sregs.fs.dpl = sregs.gs.dpl = sregs.ss.dpl = 0;
    sregs.ds.s = sregs.es.s = sregs.fs.s = sregs.gs.s = sregs.ss.s = 1;
    // In 64-bit mode only the code segment may have the L bit set.  Setting
    // L on data/stack segments makes the descriptor unusable and can cause
    // KVM to reject the entry (or immediately raise #GP) before the guest's
    // first instruction executes.
    sregs.ds.l = sregs.es.l = sregs.fs.l = sregs.gs.l = sregs.ss.l = 0;
    sregs.ds.g = sregs.es.g = sregs.fs.g = sregs.gs.g = sregs.ss.g = 1;
    }

    if (ioctl(vcpu_fd_, KVM_SET_SREGS, &sregs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_SET_SREGS failed: " + std::string(strerror(errno))};
    }

    struct kvm_regs regs{};
    memset(&regs, 0, sizeof(regs));
    regs.rflags = 0x2;
    regs.rip = config_.reset_real_mode ? 0xfff0 : config_.rip;
    regs.rsp = config_.rsp;
    regs.rdi = config_.boot_argument;

    if (ioctl(vcpu_fd_, KVM_SET_REGS, &regs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_SET_REGS failed: " + std::string(strerror(errno))};
    }

    return {VmErrorCode::SUCCESS, "VCPU " + std::to_string(config_.vcpu_id) + " initialized"};
}

VmResult VcpuThread::run() {
    static std::once_flag signal_setup;
    std::call_once(signal_setup, [] {
        struct sigaction action{};
        action.sa_handler = +[](int) {};
        sigemptyset(&action.sa_mask);
        sigaction(SIGRTMIN + 6, &action, nullptr);
    });
    if (running_.exchange(true)) {
        return {VmErrorCode::SUCCESS, "Already running"};
    }

    stop_requested_ = false;
    __atomic_store_n(&run_->immediate_exit, 0, __ATOMIC_RELEASE);
    try { thread_ = std::make_unique<std::jthread>(&VcpuThread::execution_loop, this); }
    catch (const std::exception& error) {
        running_.store(false);
        return {VmErrorCode::VCPU_CREATE_FAILED, error.what()};
    }
    return {VmErrorCode::SUCCESS, "VCPU thread started"};
}

void VcpuThread::stop() {
    stop_requested_ = true;
    if (run_) {
        __atomic_store_n(&run_->immediate_exit, 1, __ATOMIC_RELEASE);
    }
    if (thread_ && thread_->joinable() && running_.load())
        pthread_kill(thread_->native_handle(), SIGRTMIN + 6);
}

void VcpuThread::join() {
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void VcpuThread::execution_loop() {
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGRTMIN + 6);
    pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
    try {
      while (!stop_requested_.load()) {
        int ret = ioctl(vcpu_fd_, KVM_RUN, 0);
        if (ret < 0) {
            if (errno != EINTR) {
                std::lock_guard lock(result_mutex_);
                result_ = {VmErrorCode::VCPU_CREATE_FAILED, "KVM_RUN: " + std::string(strerror(errno))};
                break;
            }
            continue;
        }

        auto result = handle_exit();
        if (!result) {
            std::lock_guard lock(result_mutex_);
            result_ = result;
            break;
        }
        if (run_->exit_reason == KVM_EXIT_SHUTDOWN || run_->exit_reason == KVM_EXIT_HLT) break;
      }
    } catch (const std::exception& error) {
        std::lock_guard lock(result_mutex_);
        result_ = {VmErrorCode::VCPU_CREATE_FAILED, error.what()};
    }
    running_ = false;
}

VmResult VcpuThread::result() const {
    std::lock_guard lock(result_mutex_);
    return result_;
}

VmResult VcpuThread::handle_exit() {
    if (exit_handler_ && (run_->exit_reason == KVM_EXIT_IO || run_->exit_reason == KVM_EXIT_MMIO ||
                          run_->exit_reason == KVM_EXIT_HYPERCALL)) {
        return exit_handler_(run_);
    }

    switch (run_->exit_reason) {
    case KVM_EXIT_IO: {
        auto& io = run_->io;
        if (vm_.bridge()) {
            bindlet_bridge::BridgeEvent event{};
            event.type = bindlet_bridge::BridgeEventType::IO_ACCESS;
            event.address = io.port;
            event.size = io.size;
            event.is_write = (io.direction == KVM_EXIT_IO_OUT);
            event.vcpu_id = config_.vcpu_id;
            vm_.bridge()->dispatch_event(event);
        }
        break;
    }
    case KVM_EXIT_MMIO: {
        auto& mmio = run_->mmio;
        if (vm_.bridge()) {
            bindlet_bridge::BridgeEvent event{};
            event.type = bindlet_bridge::BridgeEventType::MMIO_ACCESS;
            event.address = mmio.phys_addr;
            event.size = mmio.len;
            event.is_write = mmio.is_write;
            event.vcpu_id = config_.vcpu_id;
            vm_.bridge()->dispatch_event(event);
        }
        break;
    }
    case KVM_EXIT_HYPERCALL:
        if (vm_.bridge()) {
            bindlet_bridge::BridgeEvent event{};
            event.type = bindlet_bridge::BridgeEventType::HYPERCALL;
            event.vcpu_id = config_.vcpu_id;
            vm_.bridge()->dispatch_event(event);
        }
        break;
    case KVM_EXIT_HLT:
        halted_.store(true);
        break;
    case KVM_EXIT_SHUTDOWN:
        return {VmErrorCode::VCPU_CREATE_FAILED, "Guest CPU shutdown (possible triple fault)"};
    case KVM_EXIT_FAIL_ENTRY:
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "VCPU failed entry: hardware_entry_failure_reason=" +
                std::to_string(run_->fail_entry.hardware_entry_failure_reason)};
    case KVM_EXIT_INTERNAL_ERROR:
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "VCPU internal error: suberror=" +
                std::to_string(run_->internal.suberror)};
    default:
        break;
    }

    return {VmErrorCode::SUCCESS, ""};
}

VmResult VcpuThread::get_regs(VcpuRegisters& regs) {
    struct kvm_regs kregs{};
    if (ioctl(vcpu_fd_, KVM_GET_REGS, &kregs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_GET_REGS failed: " + std::string(strerror(errno))};
    }
    regs.rax = kregs.rax; regs.rbx = kregs.rbx;
    regs.rcx = kregs.rcx; regs.rdx = kregs.rdx;
    regs.rsi = kregs.rsi; regs.rdi = kregs.rdi;
    regs.rsp = kregs.rsp; regs.rbp = kregs.rbp;
    regs.r8 = kregs.r8;   regs.r9 = kregs.r9;
    regs.r10 = kregs.r10; regs.r11 = kregs.r11;
    regs.r12 = kregs.r12; regs.r13 = kregs.r13;
    regs.r14 = kregs.r14; regs.r15 = kregs.r15;
    regs.rip = kregs.rip; regs.rflags = kregs.rflags;
    return {VmErrorCode::SUCCESS, ""};
}

VmResult VcpuThread::set_regs(const VcpuRegisters& regs) {
    struct kvm_regs kregs{};
    kregs.rax = regs.rax; kregs.rbx = regs.rbx;
    kregs.rcx = regs.rcx; kregs.rdx = regs.rdx;
    kregs.rsi = regs.rsi; kregs.rdi = regs.rdi;
    kregs.rsp = regs.rsp; kregs.rbp = regs.rbp;
    kregs.r8 = regs.r8;   kregs.r9 = regs.r9;
    kregs.r10 = regs.r10; kregs.r11 = regs.r11;
    kregs.r12 = regs.r12; kregs.r13 = regs.r13;
    kregs.r14 = regs.r14; kregs.r15 = regs.r15;
    kregs.rip = regs.rip; kregs.rflags = regs.rflags;
    if (ioctl(vcpu_fd_, KVM_SET_REGS, &kregs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_SET_REGS failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, ""};
}

VmResult VcpuThread::get_sregs(struct kvm_sregs& sregs) {
    if (ioctl(vcpu_fd_, KVM_GET_SREGS, &sregs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_GET_SREGS failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, ""};
}

VmResult VcpuThread::set_sregs(const struct kvm_sregs& sregs) {
    if (ioctl(vcpu_fd_, KVM_SET_SREGS, &sregs) < 0) {
        return {VmErrorCode::VCPU_CREATE_FAILED,
                "KVM_SET_SREGS failed: " + std::string(strerror(errno))};
    }
    return {VmErrorCode::SUCCESS, ""};
}

} // namespace domvirt::hypervisor
