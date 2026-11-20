"""Dependency-free client and subprocess fixture for ArchVirt OS tests.

    with TestVM("build/archvirt") as vm:
        vm.boot(machine="aarch64")
        vm.expect(b"domix# ")
        vm.send("uname\n")
        vm.expect(b"Domix aarch64\n")

One JSON request/response per Unix stream connection. All guest bytes use hex
on the wire, so console and memory contents need not be valid Unicode.
"""
from __future__ import annotations

import json
import pathlib
import shutil
import socket
import subprocess
import tempfile
import threading
import time


class IPCError(RuntimeError):
    def __init__(self, error):
        self.error = error
        super().__init__(error.get("message", str(error)) if isinstance(error, dict) else str(error))


class Client:
    def __init__(self, path, timeout=10.0):
        self.path = str(path)
        self.timeout = timeout
        self.generation = None
        self.offset = 0
        self._pending = b""
        self._id = 0
        self._request_lock = threading.Lock()

    def request(self, command, **arguments):
        with self._request_lock:
            self._id += 1
            request_id = self._id
            payload = json.dumps({"id": request_id, "execute": command, "arguments": arguments}).encode() + b"\n"
            if len(payload) > 256 * 1024:
                raise ValueError("IPC requests are limited to 256 KiB")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
                connection.settimeout(self.timeout)
                connection.connect(self.path)
                connection.sendall(payload)
                connection.shutdown(socket.SHUT_WR)
                response = bytearray()
                while b"\n" not in response:
                    chunk = connection.recv(65536)
                    if not chunk:
                        raise IPCError("Server closed the connection before completing its response")
                    response.extend(chunk)
                    if len(response) > 1024 * 1024:
                        raise IPCError("IPC response exceeds 1 MiB")
            value = json.loads(response.split(b"\n", 1)[0])
            if value.get("id") != request_id:
                raise IPCError("IPC response ID mismatch")
            if "error" in value:
                raise IPCError(value["error"])
            return value["return"]

    def _new_boot(self, result):
        self.generation, self.offset, self._pending = result["generation"], 0, b""
        return result

    def boot(self, **options):
        return self._new_boot(self.request("boot", **options))

    def reset(self, paused=False):
        return self._new_boot(self.request("reset", paused=paused))

    def status(self):
        return self.request("query-status")

    def pause(self):
        return self.request("pause")

    def resume(self):
        return self.request("resume")

    def step(self, count=1):
        return self.request("step", count=count)

    def stop(self):
        return self.request("stop")

    def registers(self):
        return self.request("query-registers")["registers"]

    def read_memory(self, address, size):
        return bytes.fromhex(self.request("read-memory", address=address, size=size)["data_hex"])

    def write_memory(self, address, data):
        return self.request("write-memory", address=address, data_hex=bytes(data).hex())["written"]

    def screen(self):
        result = self.request("query-screen")
        result["data"] = bytes.fromhex(result.pop("data_hex"))
        return result

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        return self.request("console-write", data_hex=bytes(data).hex())["accepted"]

    def _receive(self):
        args = {"offset": self.offset, "limit": 65536}
        if self.generation is not None:
            args["generation"] = self.generation
        result = self.request("console-read", **args)
        self.generation, self.offset = result["generation"], result["next"]
        if result["truncated"]:
            raise IPCError("Console capture overflowed; output was lost")
        return bytes.fromhex(result["data_hex"])

    def read_console(self):
        result = self._pending + self._receive()
        self._pending = b""
        return result

    def expect(self, marker, timeout=5.0):
        """Consume through a byte/string marker, retaining any following output.

        Raises TimeoutError or IPCError with the output tail and VM state.
        To assert that a guest halts successfully, use wait() and inspect result.
        """
        if isinstance(marker, str):
            marker = marker.encode("utf-8")
        if not marker:
            raise ValueError("Expected marker must not be empty")
        deadline = time.monotonic() + timeout
        while True:
            found = self._pending.find(marker)
            if found >= 0:
                end = found + len(marker)
                result, self._pending = self._pending[:end], self._pending[end:]
                return result
            if time.monotonic() >= deadline:
                state = self.status()
                raise TimeoutError(f"Waiting for {marker!r}; state={state['state']}; output tail={self._pending[-512:]!r}")
            data = self._receive()
            self._pending += data
            if len(self._pending) > 1024 * 1024:
                raise IPCError("Expected marker not found within 1 MiB of console output")
            if data:
                continue
            state = self.status()
            if state["result"]["complete"]:
                # Drain once more: the CPU may have halted between read/status.
                data = self._receive()
                if data:
                    self._pending += data
                    continue
                raise IPCError(f"Guest {state['state']} before {marker!r}: {state['result']}; output tail={self._pending[-512:]!r}")
            time.sleep(0.01)

    def wait(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while True:
            state = self.status()
            if state["result"]["complete"]:
                return state
            if time.monotonic() >= deadline:
                raise TimeoutError(f"Guest did not finish: {state}")
            time.sleep(0.01)


class TestVM(Client):
    """Context manager owning a test-server process and its temporary socket.

    Extra CLI arguments set defaults for subsequent boot() calls. Tests drive
    the guest entirely via IPC; stdout is discarded and stderr is kept in a
    temporary file, avoiding pipe backpressure on chatty kernels.
    """
    __test__ = False  # pytest should not collect this fixture as a test class.

    def __init__(self, binary="archvirt", *arguments, timeout=10.0):
        self.binary = shutil.which(str(binary)) or str(pathlib.Path(binary).resolve())
        self.arguments = arguments
        self.timeout = timeout
        self.process = None
        self._directory = None
        self._stderr = None

    def __enter__(self):
        self._directory = tempfile.TemporaryDirectory(prefix="archvirt-test-")
        self._stderr = tempfile.TemporaryFile()
        super().__init__(pathlib.Path(self._directory.name) / "vm.sock", self.timeout)
        try:
            self.process = subprocess.Popen([self.binary, *self.arguments, "--test-server", self.path],
                                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=self._stderr)
            deadline = time.monotonic() + self.timeout
            while True:
                if self.process.poll() is not None:
                    self._stderr.seek(0)
                    raise IPCError("Test server exited: " + self._stderr.read().decode(errors="replace"))
                try:
                    self.request("query-capabilities")
                    return self
                except (FileNotFoundError, ConnectionRefusedError):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("Test server did not become ready")
                    time.sleep(0.01)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.process is not None and self.process.poll() is None:
            try:
                self.request("quit")
                self.process.wait(timeout=self.timeout)
            except (OSError, IPCError, subprocess.TimeoutExpired):
                self.process.terminate()
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
        if self._stderr is not None:
            self._stderr.close()
            self._stderr = None
        if self._directory is not None:
            self._directory.cleanup()
            self._directory = None

    def __exit__(self, exc_type, exc, tb):
        self.close()


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Send one ArchVirt IPC request")
    parser.add_argument("socket")
    parser.add_argument("command")
    parser.add_argument("arguments", nargs="?", default="{}", help="JSON argument object")
    args = parser.parse_args()
    print(json.dumps(Client(args.socket).request(args.command, **json.loads(args.arguments)), indent=2))
