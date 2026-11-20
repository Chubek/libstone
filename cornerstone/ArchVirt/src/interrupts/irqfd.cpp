#include "irqchip.hpp"
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cerrno>

namespace domvirt::interrupts {

struct IrqFd {
    int event_fd;
    uint32_t gsi;
    int vm_fd;

    static VmResult create(int vm_fd, uint32_t gsi, int& out_fd) {
        int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (efd < 0) {
            return {VmErrorCode::INTERRUPT_ERROR,
                    "eventfd creation failed: " + std::string(strerror(errno))};
        }

        struct kvm_irqfd irqfd_data{};
        irqfd_data.fd = efd;
        irqfd_data.gsi = gsi;
        irqfd_data.flags = 0;

        if (ioctl(vm_fd, KVM_IRQFD, &irqfd_data) < 0) {
            close(efd);
            return {VmErrorCode::INTERRUPT_ERROR,
                    "KVM_IRQFD failed: " + std::string(strerror(errno))};
        }

        out_fd = efd;
        return {VmErrorCode::SUCCESS, "IRQ fd created"};
    }

    static void signal(int event_fd, uint64_t count = 1) {
        write(event_fd, &count, sizeof(count));
    }

    static void close_fd(int event_fd) {
        if (event_fd >= 0) close(event_fd);
    }
};

} // namespace domvirt::interrupts
