"""Drive real kernels entirely through IPC, including native KVM when available."""
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
from archvirt_ipc import IPCError, TestVM

binary, machine, backend = sys.argv[1:]
if backend == "kvm" and not os.access("/dev/kvm", os.R_OK | os.W_OK):
    sys.exit(77)
for name in list(os.environ):
    if name.startswith(("DOMVIRT_", "ARCHVIRT_")):
        del os.environ[name]


def rejected(call, message=None):
    try:
        call()
    except IPCError as error:
        if message:
            assert message.lower() in str(error).lower(), error
        return
    raise AssertionError("Invalid operation succeeded")


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def elf(path, code):
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIQQQIHHHHHH", ident, 2, {"x86_64":62,"aarch64":183,"riscv64":243}[machine],
                         1, 0x200000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    segment = struct.pack("<IIQQQQQQ", 1, 5, 4096, 0x200000, 0x200000, len(code), len(code), 4096)
    path.write_bytes((header + segment).ljust(4096, b"\0") + code)
    return str(path)


with tempfile.TemporaryDirectory() as directory, TestVM(binary, "--machine", machine, "--backend", backend) as vm:
    capabilities = vm.request("query-capabilities")
    assert capabilities["managed"] and capabilities["protocol_version"] == 1
    assert "riscv64" in capabilities["machines"]
    assert vm.status()["state"] == "empty"
    assert pathlib.Path(vm.path).stat().st_mode & 0o777 == 0o600
    rejected(vm.reset, "No successful boot")
    rejected(vm.registers, "not initialized")
    rejected(lambda: vm.request("not-stop"), "Unsupported command")

    # The server must not remove an existing socket when another runner binds it.
    collision = subprocess.run([binary, "--test-server", vm.path], capture_output=True, timeout=5)
    assert collision.returncode != 0 and vm.status()["state"] == "empty"
    occupied = pathlib.Path(directory) / "occupied"
    occupied.write_text("keep")
    collision = subprocess.run([binary, "--test-server", str(occupied)], capture_output=True, timeout=5)
    assert collision.returncode != 0 and occupied.read_text() == "keep"

    # Idle and fragmented connections must not monopolize the command channel.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as partial:
        partial.settimeout(5)
        partial.connect(vm.path)
        partial.sendall(b'{"id":"split","execute":')
        assert vm.status()["state"] == "empty"
        partial.sendall(b'"query-status"}\n')
        assert json.loads(partial.recv(4096))["id"] == "split"
    for payload in (b"[]\n", b"{oops\n", b'{"id":7,"execute":false}\n', b'\xff\n',
                    b'{"execute":"\xff"}\n', b" " * (256 * 1024) + b"\n"):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(5)
            connection.connect(vm.path)
            connection.sendall(payload)
            assert "error" in json.loads(connection.recv(4096))
    rejected(lambda: vm.request("console-read", limit=-1), "non-negative")
    rejected(lambda: vm.request("query-status", unexpected=True), "Unknown argument")

    # Start before the first guest instruction, inspect the ABI, and edit RAM.
    status = vm.boot(paused=True, cmdline="ipc-test=1")
    assert status["state"] == "paused" and status["instructions"] == 0
    assert status["backend"] == backend
    registers = vm.registers()
    assert registers[{"x86_64":"rdi", "aarch64":"x0", "riscv64":"x10"}[machine]] == 0x10000
    if machine == "riscv64":
        assert registers["x0"] == 0 and registers["x2"] == registers["sp"] == 0x700000
    assert vm.read_memory(0x10000, 8) == b"ARVBOOT1"
    assert vm.read_memory(0x11000, 11) == b"ipc-test=1\0"
    payload = bytes(range(256)) * 256
    assert vm.write_memory(0x300000, payload) == len(payload)
    assert vm.read_memory(0x300000, len(payload)) == payload
    rejected(lambda: vm.read_memory(2**64 - 1, 4), "outside")
    rejected(lambda: vm.read_memory(64 * 1024 * 1024 - 1, 2), "outside")
    rejected(lambda: vm.read_memory(0, 65537), "1..65536")
    rejected(lambda: vm.write_memory(0, b""), "1..65536")
    rejected(lambda: vm.request("write-memory", address=0, data_hex="zz"), "hexadecimal")
    assert vm.send(payload) == len(payload)
    rejected(lambda: vm.send(b"x"), "queue is full")
    generation = status["generation"]
    status = vm.reset(paused=True)
    assert status["generation"] == generation + 1
    rejected(lambda: vm.request("console-read", generation=generation), "generation changed")
    if backend == "software":
        assert vm.step()["instructions"] == 1
        assert vm.registers()["pc"] != registers["pc"]
        rejected(lambda: vm.step(0), "1..10000")
    else:
        rejected(vm.step, "software")
    vm.resume()
    vm.expect("domix# ", timeout=15)
    vm.send("uname\ncmdline\necho ipc-content > /ipc-note\ncat /ipc-note\n")
    vm.expect(f"Domix {machine}\n", timeout=10)
    vm.expect("ipc-test=1\n")
    vm.expect("domix# cat /ipc-note\nipc-content\n")
    rejected(vm.registers, "Pause")
    rejected(lambda: vm.read_memory(0, 1), "Pause")
    rejected(lambda: vm.write_memory(0, b"x"), "Pause")
    for _ in range(8):
        status = vm.pause()
        assert status["state"] == "paused" and status["active"] and not status["running"]
        registers, count = vm.registers(), status["instructions"]
        time.sleep(0.002)
        assert vm.registers() == registers and vm.status()["instructions"] == count
        screen = vm.screen()
        assert len(screen["data"]) == 80 * 25 * 2 and screen["format"] == "vga-text"
        vm.resume()
    try:
        vm.expect("a-marker-never-printed", timeout=0.05)
        raise AssertionError("expect did not time out")
    except TimeoutError:
        pass
    vm.send("exit\n")
    vm.expect("Domix shutdown\n")
    status = vm.wait()
    assert status["state"] == "halted" and status["result"]["code"] == 0, status
    assert vm.registers()["pc"] >= 0x200000
    vm.reset()
    vm.expect("domix# ", timeout=15)
    vm.send("cat /ipc-note\n")
    vm.expect("file not found\n")
    assert vm.stop()["state"] == "stopped"

    # A developer-provided ELF, binary console bytes, and exact software steps.
    path = pathlib.Path(directory) / "kernel.elf"
    if machine == "x86_64":
        code = b"\x90\xb8\x2a\0\0\0\xba\xf8\x03\0\0\xee\xb0\0\xee\xb0\xff\xee\xf4"
        loop, fault = b"\xeb\xfe", b"\x0f\x0b"
    elif machine == "riscv64":
        # NOP; ADDI x1,zero,42; LUI x2,0x10000; SB x1,0(x2); ...; EBREAK.
        code = words(0x00000013, 0x02a00093, 0x10000137, 0x00110023,
                     0x00000093, 0x00110023, 0x0ff00093, 0x00110023, 0x00100073)
        loop, fault = words(0x0000006f), words(0xffffffff)
    else:
        code = words(0xd503201f, 0xd2800541, 0xd2a20002, 0x39000041,
                     0xd2800001, 0x39000041, 0xd2801fe1, 0x39000041, 0xd4400000)
        loop, fault = words(0x14000000), words(0xffffffff)
    image = pathlib.Path(directory) / "data.img"
    image.write_bytes(b"custom-image\0\xff")
    vm.boot(kernel=elf(path, code), image=str(image), paused=True)
    assert vm.read_memory(0x800000, 14) == image.read_bytes()
    if backend == "software":
        assert vm.step(2)["instructions"] == 2
        assert vm.registers()["rax" if machine == "x86_64" else "x1"] == 42
        assert vm.step(20)["state"] == "halted"
    else:
        vm.resume()
        assert vm.wait()["state"] == "halted"
    assert vm.read_console() == b"*\0\xff"
    # Halt is also reported by expect when the requested output never arrives.
    rejected(lambda: vm.expect("missing"), "halted")

    # Pure compute must remain interruptible, even in KVM_RUN without I/O exits.
    vm.boot(kernel=elf(path, loop))
    assert vm.pause()["state"] == "paused"
    vm.resume()
    assert vm.stop()["state"] == "stopped"
    if backend == "software":
        vm.boot(kernel=str(path), max_instructions=13)
        status = vm.wait()
        assert status["state"] == "failed" and status["instructions"] == 13, status
        assert "limit" in status["result"]["message"]
        vm.boot(kernel=elf(path, fault), paused=True)
        rejected(vm.step)
        assert vm.status()["state"] == "failed"
        vm.reset()
        assert vm.wait()["state"] == "failed"
    rejected(lambda: vm.boot(machine="unsupported"), "Unsupported machine")
    status = vm.status()
    assert status["state"] == "empty" and status["result"]["complete"] and status["result"]["code"] != 0
    # A failed boot does not poison the persistent server or last valid config.
    vm.boot(kernel=elf(path, code))
    assert vm.wait()["state"] == "halted"

    # Capturing more than the ring capacity must expose loss, never silently pass.
    if backend == "software":
        if machine == "x86_64":
            flood = b"\xba\xf8\x03\0\0\xb8A\0\0\0\xb9\x20\0\x10\0\xee\xff\xc9\x75\xfb\xf4"
        elif machine == "riscv64":
            # 0x100020 byte writes, decrement and branch back to SB.
            flood = words(0x10000137, 0x04100093, 0x001001b7, 0x02018193,
                          0x00110023, 0xfff18193, 0xfe019ce3, 0x00100073)
        else:
            flood = words(0xd2a20002, 0xd2800821, 0xd2800403, 0xf2a00203,
                          0x39000041, 0xf1000463, 0x54ffffc1, 0xd4400000)
        vm.boot(kernel=elf(path, flood), max_instructions=5000000)
        assert vm.wait(timeout=25)["state"] == "halted"
        capture = vm.request("console-read", offset=0, limit=7)
        assert capture["begin"] == 32 and capture["end"] == 1048576 + 32, capture
        assert capture["truncated"] and bytes.fromhex(capture["data_hex"]) == b"A" * 7
        rejected(vm.read_console, "overflowed")

    # Shutdown still works while a client holds an incomplete request open.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as idle:
        idle.connect(vm.path)
        idle.sendall(b"{")
        vm.request("quit")
        assert vm.process.wait(timeout=5) == 0
    assert not pathlib.Path(vm.path).exists()
print(f"{machine}/{backend}: IPC boot, console, inspection, lifecycle and failure handling passed")
