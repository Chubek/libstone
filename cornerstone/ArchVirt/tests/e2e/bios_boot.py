#!/usr/bin/env python3
"""Reset/ROM/interrupt/disk acceptance using the actual native guest over IPC."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from archvirt_ipc import TestVM, IPCError


def check(binary):
    with tempfile.TemporaryDirectory(prefix="bios-acceptance-") as tmp, TestVM(binary) as vm:
        disk = Path(tmp) / "probe.img"
        subprocess.run(["nasm", "-f", "bin", str(ROOT / "bios/probe.asm"), "-o", str(disk)], check=True)
        digest = hashlib.sha256(disk.read_bytes()).digest()
        for memory in (40, 64, 128):
            vm.boot(machine="x86_64", backend="kvm", boot_mode="bios", disk=str(disk), memory_mb=memory, paused=True)
            regs = vm.registers()
            assert (regs["cs"],regs["cs_base"],regs["rip"],regs["cr0"] & 0x80000001,regs["efer"]) == (0xf000,0xffff0000,0xfff0,0,0), regs
            rom = vm.read_memory(0xf0000,65536)
            assert rom == vm.read_memory(0xffff0000,65536) and sum(rom) % 256 == 0
            try: vm.write_memory(0xf0000,b"\0")
            except IPCError: pass
            else: raise AssertionError("IPC overwrote ROM")
            vm.resume()
            vm.expect("BIOS PROBE INPUT\r\n")
            vm.send("Z")
            vm.expect("BIOS PROBE PASS\r\n")
            state = vm.wait()
            assert state["state"] == "halted" and state["result"]["code"] == 0, state
            expected=[(0,0x9f000,1,1),(0x9f000,0x61000,2,1),(0x100000,memory*1024*1024-0x100000,1,1),(0xffff0000,65536,2,1)]
            assert list(struct.iter_unpack("<QQII",vm.read_memory(0x6000,96))) == expected
            assert vm.read_memory(0x6100,4) == b"SSAP"
            assert vm.read_memory(0x7000,4) == bytes.fromhex("21436587")
            assert b"BIOS PROBE PASS" in vm.screen()["data"][::2]
            assert hashlib.sha256(disk.read_bytes()).digest() == digest
        # No boot signature is rejected by firmware, not the host ELF loader.
        raw = bytearray(disk.read_bytes()); raw[510:512] = b"\0\0"; disk.write_bytes(raw)
        vm.boot(backend="kvm",boot_mode="bios",disk=str(disk))
        vm.expect("BIOS: no bootable disk\r\n"); assert vm.wait()["state"] == "halted"
        subprocess.run(["nasm","-DROM_WRITE","-f","bin",str(ROOT/"bios/probe.asm"),"-o",str(disk)],check=True)
        vm.boot(backend="kvm",boot_mode="bios",disk=str(disk))
        assert vm.wait()["state"] == "failed"
        assert vm.read_memory(0xf0000,1) == b"\xfa"
        for options in ({"backend":"software"},{"machine":"riscv64"},{"max_instructions":1},{"kernel":"unused"}):
            args={"backend":"kvm", "boot_mode":"bios", "disk":str(disk), **options}
            try: vm.boot(**args)
            except IPCError: pass
            else: raise AssertionError(options)
    print("BIOS B1: reset, aliases/checksum, CHS/EDD, volatile writes, invalid transfers, E820, keyboard, VGA, signature and ROM protection passed")


if __name__ == "__main__":
    if not os.access("/dev/kvm", os.R_OK | os.W_OK): sys.exit(77)
    check(sys.argv[1])
