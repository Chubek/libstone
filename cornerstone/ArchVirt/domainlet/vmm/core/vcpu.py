"""vCPU execution thread."""

from __future__ import annotations

import ctypes
import mmap
import os
import threading
from dataclasses import dataclass, field

from domainlet.vmm.core.exits import ExitDispatcher, VmExitReason
from domainlet.vmm.core.kvm.bindings import KVM_EXIT_IO, KVM_EXIT_MMIO, KVM_RUN, ioctl, kvm_run
from domainlet.vmm.errors import VmmError


@dataclass(slots=True)
class Vcpu:
    """A single runnable vCPU."""

    fd: int
    mmap_size: int
    bus: object
    dispatcher: ExitDispatcher = field(default_factory=ExitDispatcher)
    _run_page: mmap.mmap = field(init=False, repr=False)
    _run: kvm_run = field(init=False, repr=False)
    _thread: threading.Thread | None = field(
        default=None, init=False, repr=False)
    _stop: threading.Event = field(
        default_factory=threading.Event, init=False, repr=False)

    def __post_init__(self) -> None:
        self._run_page = mmap.mmap(
            self.fd, self.mmap_size, access=mmap.ACCESS_WRITE)
        self._run = kvm_run.from_buffer(self._run_page)

    def close(self) -> None:
        """Stop the vCPU and release the run page."""

        self.stop()
        self._run_page.close()
        os.close(self.fd)

    def stop(self) -> None:
        """Ask the execution loop to stop."""

        self._stop.set()
        self._run.immediate_exit = 1

    def start(self) -> None:
        """Run the vCPU loop in a background thread."""

        if self._thread is not None:
            return
        self._thread = threading.Thread(
            target=self.run, name=f"vcpu-{self.fd}", daemon=True)
        self._thread.start()

    def join(self, timeout: float | None = None) -> None:
        """Wait for the background thread."""

        if self._thread is not None:
            self._thread.join(timeout)

    def run(self) -> None:
        """Execute until stopped or a fatal exit occurs."""

        while not self._stop.is_set():
            ioctl(self.fd, KVM_RUN)
            reason = self._run.exit_reason
            if reason == KVM_EXIT_MMIO:
                payload = bytes(self._run.mmio.data[: self._run.mmio.len])
                self.bus.dispatch(self._run.mmio.phys_addr,
                                  payload, write=bool(self._run.mmio.is_write))
            elif reason == KVM_EXIT_IO:
                view = memoryview(self._run_page)
                start = int(self._run.io.data_offset)
                stop = start + int(self._run.io.size) * int(self._run.io.count)
                payload = bytes(view[start:stop])
                self.bus.dispatch(self._run.io.port, payload,
                                  write=self._run.io.direction == 1)
            elif reason in (VmExitReason.HLT, VmExitReason.SHUTDOWN):
                break
            else:
                raise VmmError(f"unsupported KVM exit reason {reason}")
