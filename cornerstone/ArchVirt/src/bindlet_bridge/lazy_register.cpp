#include "lazy_register.hpp"
#include <sys/ioctl.h>
#include <linux/kvm.h>
#include <Python.h>
#include <cstring>
#include <cerrno>

namespace domvirt::bindlet_bridge {

LazyRegisterProxy::LazyRegisterProxy() {
    memset(&regs_, 0, sizeof(regs_));
#if defined(__x86_64__)
    memset(&sregs_, 0, sizeof(sregs_));
#endif
}

LazyRegisterProxy::~LazyRegisterProxy() = default;

void LazyRegisterProxy::update_from_kvm(int vcpu_fd) {
#if defined(__x86_64__)
    struct kvm_regs kregs{};
    if (ioctl(vcpu_fd, KVM_GET_REGS, &kregs) >= 0) {
        regs_.rax = kregs.rax; regs_.rbx = kregs.rbx;
        regs_.rcx = kregs.rcx; regs_.rdx = kregs.rdx;
        regs_.rsi = kregs.rsi; regs_.rdi = kregs.rdi;
        regs_.rsp = kregs.rsp; regs_.rbp = kregs.rbp;
        regs_.r8 = kregs.r8;   regs_.r9 = kregs.r9;
        regs_.r10 = kregs.r10; regs_.r11 = kregs.r11;
        regs_.r12 = kregs.r12; regs_.r13 = kregs.r13;
        regs_.r14 = kregs.r14; regs_.r15 = kregs.r15;
        regs_.rip = kregs.rip; regs_.rflags = kregs.rflags;
    }

    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs_) >= 0) {
        regs_.cr0 = sregs_.cr0; regs_.cr2 = sregs_.cr2;
        regs_.cr3 = sregs_.cr3; regs_.cr4 = sregs_.cr4;
        regs_.cr8 = sregs_.cr8;
        regs_.efer = sregs_.efer;
        regs_.apic_base = sregs_.apic_base;
    }

    synced_ = true;
    mark_clean(RegisterClass::ALL);
#endif
}

void LazyRegisterProxy::commit_to_kvm(int vcpu_fd) {
#if defined(__x86_64__)
    if (!synced_) return;

    if (is_dirty(RegisterClass::GPR)) {
        struct kvm_regs kregs{};
        kregs.rax = regs_.rax; kregs.rbx = regs_.rbx;
        kregs.rcx = regs_.rcx; kregs.rdx = regs_.rdx;
        kregs.rsi = regs_.rsi; kregs.rdi = regs_.rdi;
        kregs.rsp = regs_.rsp; kregs.rbp = regs_.rbp;
        kregs.r8 = regs_.r8;   kregs.r9 = regs_.r9;
        kregs.r10 = regs_.r10; kregs.r11 = regs_.r11;
        kregs.r12 = regs_.r12; kregs.r13 = regs_.r13;
        kregs.r14 = regs_.r14; kregs.r15 = regs_.r15;
        kregs.rip = regs_.rip; kregs.rflags = regs_.rflags;
        ioctl(vcpu_fd, KVM_SET_REGS, &kregs);
    }

    if (is_dirty(RegisterClass::CONTROL)) {
        sregs_.cr0 = regs_.cr0; sregs_.cr2 = regs_.cr2;
        sregs_.cr3 = regs_.cr3; sregs_.cr4 = regs_.cr4;
        sregs_.cr8 = regs_.cr8;
        sregs_.efer = regs_.efer;
        sregs_.apic_base = regs_.apic_base;
        ioctl(vcpu_fd, KVM_SET_SREGS, &sregs_);
    }

    mark_clean(RegisterClass::ALL);
#endif
}

PyObject* LazyRegisterProxy::to_python() {
    PyObject* dict = PyDict_New();
    if (!dict) return nullptr;

    PyDict_SetItemString(dict, "rax", PyLong_FromUnsignedLongLong(regs_.rax));
    PyDict_SetItemString(dict, "rbx", PyLong_FromUnsignedLongLong(regs_.rbx));
    PyDict_SetItemString(dict, "rcx", PyLong_FromUnsignedLongLong(regs_.rcx));
    PyDict_SetItemString(dict, "rdx", PyLong_FromUnsignedLongLong(regs_.rdx));
    PyDict_SetItemString(dict, "rsi", PyLong_FromUnsignedLongLong(regs_.rsi));
    PyDict_SetItemString(dict, "rdi", PyLong_FromUnsignedLongLong(regs_.rdi));
    PyDict_SetItemString(dict, "rsp", PyLong_FromUnsignedLongLong(regs_.rsp));
    PyDict_SetItemString(dict, "rbp", PyLong_FromUnsignedLongLong(regs_.rbp));
    PyDict_SetItemString(dict, "r8",  PyLong_FromUnsignedLongLong(regs_.r8));
    PyDict_SetItemString(dict, "r9",  PyLong_FromUnsignedLongLong(regs_.r9));
    PyDict_SetItemString(dict, "r10", PyLong_FromUnsignedLongLong(regs_.r10));
    PyDict_SetItemString(dict, "r11", PyLong_FromUnsignedLongLong(regs_.r11));
    PyDict_SetItemString(dict, "r12", PyLong_FromUnsignedLongLong(regs_.r12));
    PyDict_SetItemString(dict, "r13", PyLong_FromUnsignedLongLong(regs_.r13));
    PyDict_SetItemString(dict, "r14", PyLong_FromUnsignedLongLong(regs_.r14));
    PyDict_SetItemString(dict, "r15", PyLong_FromUnsignedLongLong(regs_.r15));
    PyDict_SetItemString(dict, "rip", PyLong_FromUnsignedLongLong(regs_.rip));
    PyDict_SetItemString(dict, "rflags", PyLong_FromUnsignedLongLong(regs_.rflags));
    PyDict_SetItemString(dict, "cr0", PyLong_FromUnsignedLongLong(regs_.cr0));
    PyDict_SetItemString(dict, "cr2", PyLong_FromUnsignedLongLong(regs_.cr2));
    PyDict_SetItemString(dict, "cr3", PyLong_FromUnsignedLongLong(regs_.cr3));
    PyDict_SetItemString(dict, "cr4", PyLong_FromUnsignedLongLong(regs_.cr4));
    PyDict_SetItemString(dict, "cr8", PyLong_FromUnsignedLongLong(regs_.cr8));
    PyDict_SetItemString(dict, "efer", PyLong_FromUnsignedLongLong(regs_.efer));

    return dict;
}

void LazyRegisterProxy::from_python(PyObject* obj) {
    if (!PyDict_Check(obj)) return;

    auto get_u64 = [&](const char* key) -> uint64_t {
        PyObject* val = PyDict_GetItemString(obj, key);
        if (val && PyLong_Check(val)) return PyLong_AsUnsignedLongLong(val);
        return 0;
    };

    regs_.rax = get_u64("rax"); regs_.rbx = get_u64("rbx");
    regs_.rcx = get_u64("rcx"); regs_.rdx = get_u64("rdx");
    regs_.rsi = get_u64("rsi"); regs_.rdi = get_u64("rdi");
    regs_.rsp = get_u64("rsp"); regs_.rbp = get_u64("rbp");
    regs_.r8 = get_u64("r8");   regs_.r9 = get_u64("r9");
    regs_.r10 = get_u64("r10"); regs_.r11 = get_u64("r11");
    regs_.r12 = get_u64("r12"); regs_.r13 = get_u64("r13");
    regs_.r14 = get_u64("r14"); regs_.r15 = get_u64("r15");
    regs_.rip = get_u64("rip"); regs_.rflags = get_u64("rflags");
    regs_.cr0 = get_u64("cr0"); regs_.cr2 = get_u64("cr2");
    regs_.cr3 = get_u64("cr3"); regs_.cr4 = get_u64("cr4");
    regs_.cr8 = get_u64("cr8"); regs_.efer = get_u64("efer");

    mark_dirty(RegisterClass::GPR | RegisterClass::CONTROL);
}

} // namespace domvirt::bindlet_bridge
