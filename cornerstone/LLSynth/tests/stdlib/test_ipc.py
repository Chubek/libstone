"""Exercise std.ipc through the generated Python and native C tool targets."""

import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
from uuid import uuid4


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"
CC = shlex.split(os.environ.get("CC", "cc"))
HAS_CC = bool(CC and shutil.which(CC[0]))
LINK = ["-pthread", "-lm"] + (["-lrt"] if sys.platform.startswith("linux") else [])

SPEC = r'''.language "IPCTest"
.import std.ipc as IPC
.token CMD: String = /[a-z-]+/
.token NAME: String = /\/[^ \t\n]+/
.token FD: Int = /[0-9]+/
.skip /[ \t\n]+/
.exception !Mismatch "unexpected IPC result"
.rule Start
'syntax: cmd:CMD name:NAME? inherited:FD? EOF
'semantics:
if $cmd == "supports-pipe" {
    if IPC:available("pipe") { return 1; }
    return 0;
}
if $cmd == "supports-sem" {
    if IPC:available("sem") { return 1; }
    return 0;
}
if $cmd == "pipe" {
    let make: Fn()->Vec<Int> = &IPC->pipe;
    let ends = make();
    let r = ends[0];
    let w = ends[1];
    discard IPC:setNonblocking(r, true);
    require IPC:waitReadable(r, 5) == false && IPC:waitWritable(w, 0) else Throw(!Mismatch);
    require IPC:write(w, [0, 255, 3]) == 3 else Throw(!Mismatch);
    require IPC:waitReadable(r, 0) else Throw(!Mismatch);
    require IPC:read(r, 16) == [0, 255, 3] else Throw(!Mismatch);
    discard IPC:close(w);
    require IPC:waitReadable(r, 50) && IPC:read(r, 16) == [] else Throw(!Mismatch);
    discard IPC:close(r);
    return 42;
}
if $cmd == "would-block" {
    let ends = IPC:pipe();
    discard IPC:setNonblocking(ends[0], true);
    return len(IPC:read(ends[0], 1));
}
if $cmd == "broken-pipe" {
    let ends = IPC:pipe();
    discard IPC:close(ends[0]);
    return IPC:write(ends[1], [1]);
}
if $cmd == "invalid-bytes" {
    let socket = IPC:socketPair();
    return IPC:write(socket[0], [256]);
}
if $cmd == "invalid-descriptor" {
    discard IPC:close(-1);
    return 0;
}
if $cmd == "invalid-size" {
    let ends = IPC:pipe();
    return len(IPC:read(ends[0], 1048577));
}
if $cmd == "read-inherited" {
    let data = IPC:read($inherited, 16);
    discard IPC:close($inherited);
    require data == [48, 0, 255] else Throw(!Mismatch);
    return 42;
}
if $cmd == "socketpair" {
    let ends = IPC:socketPair();
    let left = ends[0];
    let right = ends[1];
    require IPC:write(left, [42]) == 1 && IPC:read(right, 8) == [42] else Throw(!Mismatch);
    require IPC:write(right, [7, 0]) == 2 && IPC:read(left, 8) == [7, 0] else Throw(!Mismatch);
    discard IPC:close(right);
    discard IPC:close(left);
    return 42;
}
if $cmd == "fifo" {
    discard IPC:fifoCreate($name);
    let reader = IPC:fifoOpen($name, false);
    let writer = IPC:fifoOpen($name, true);
    discard IPC:fifoUnlink($name);
    require IPC:write(writer, [42, 0, 255]) == 3 else Throw(!Mismatch);
    require IPC:waitReadable(reader, 200) && IPC:read(reader, 16) == [42, 0, 255] else Throw(!Mismatch);
    discard IPC:close(writer);
    discard IPC:close(reader);
    return 42;
}
if $cmd == "fifo-unlink" {
    discard IPC:fifoUnlink($name);
    return 42;
}
if $cmd == "fifo-create" {
    discard IPC:fifoCreate($name);
    return 42;
}
if $cmd == "fifo-read" {
    let fd = IPC:fifoOpen($name, false);
    require IPC:waitReadable(fd, 200) && IPC:read(fd, 16) == [42, 0, 255] else Throw(!Mismatch);
    discard IPC:close(fd);
    return 42;
}
if $cmd == "mq-produce" {
    let queue = IPC:mqCreate($name, 2, 16, true);
    discard IPC:mqSend(queue, [], 0);
    discard IPC:mqSend(queue, [0, 255, 42], 1);
    discard IPC:mqClose(queue);
    return 42;
}
if $cmd == "mq-consume" {
    let queue = IPC:mqOpen($name, true);
    require IPC:mqReceive(queue) == [0, 255, 42] && IPC:mqReceive(queue) == [] else Throw(!Mismatch);
    discard IPC:mqClose(queue);
    return 42;
}
if $cmd == "mq-empty" {
    let queue = IPC:mqOpen($name, true);
    return len(IPC:mqReceive(queue));
}
if $cmd == "mq-unlink" {
    discard IPC:mqUnlink($name);
    return 42;
}
if $cmd == "shm-produce" {
    let shared = IPC:shmCreate($name, 8);
    require IPC:shmSize(shared) == 8 && IPC:shmWrite(shared, 2, [0, 255, 42]) == 3 else Throw(!Mismatch);
    discard IPC:close(shared);
    return 42;
}
if $cmd == "shm-consume" {
    let shared = IPC:shmOpen($name, false);
    require IPC:shmRead(shared, 0, 8) == [0, 0, 0, 255, 42, 0, 0, 0] else Throw(!Mismatch);
    require IPC:shmRead(shared, 8, 12) == [] else Throw(!Mismatch);
    discard IPC:close(shared);
    return 42;
}
if $cmd == "shm-out-of-bounds" {
    let shared = IPC:shmOpen($name, true);
    return IPC:shmWrite(shared, 7, [1, 2]);
}
if $cmd == "shm-unlink" {
    discard IPC:shmUnlink($name);
    return 42;
}
if $cmd == "sem-create" {
    let sem = IPC:semCreate($name, 1);
    require IPC:semTryWait(sem) && IPC:semTryWait(sem) == false else Throw(!Mismatch);
    discard IPC:semClose(sem);
    return 42;
}
if $cmd == "sem-produce" {
    let sem = IPC:semOpen($name);
    discard IPC:semPost(sem);
    discard IPC:semPost(sem);
    discard IPC:semClose(sem);
    return 42;
}
if $cmd == "sem-consume" {
    let sem = IPC:semOpen($name);
    require IPC:semTryWait(sem) && IPC:semTryWait(sem) && IPC:semTryWait(sem) == false else Throw(!Mismatch);
    discard IPC:semClose(sem);
    return 42;
}
if $cmd == "sem-hold-unlink" {
    let first = IPC:semOpen($name);
    let second = IPC:semOpen($name);
    discard IPC:semUnlink($name);
    discard IPC:semClose(first);
    discard IPC:semPost(second);
    require IPC:semTryWait(second) else Throw(!Mismatch);
    discard IPC:semClose(second);
    return 42;
}
if $cmd == "sem-unlink" {
    discard IPC:semUnlink($name);
    return 42;
}
if $cmd == "sem-stale" {
    let first = IPC:semOpen($name);
    discard IPC:semClose(first);
    let second = IPC:semOpen($name);
    discard IPC:semPost(first);
    discard IPC:semClose(second);
    return 42;
}
if $cmd == "sem-invalid" {
    discard IPC:semPost(42);
    return 42;
}
if $cmd == "sem-negative" {
    discard IPC:semCreate($name, -1);
    return 42;
}
return -1;
.end
'''


class IPCTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory(prefix="llsynth-ipc-")
        cls.addClassCleanup(directory.cleanup)
        cls.work = Path(directory.name)
        cls.spec = cls.work / "ipc.llsynth"
        cls.spec.write_text(SPEC, encoding="utf-8")
        cls.commands = {"python3": [sys.executable, cls.generate("python3")]}
        if HAS_CC and os.name == "posix":
            for target, split in (("c99", "none"), ("c11", "all")):
                source = cls.generate(target, split)
                sources = list(source.parent.glob("*.c"))
                program = source.with_suffix("")
                cls.checked([*CC, f"-std={target}", "-pedantic-errors", *sources, *LINK, "-o", program])
                cls.commands[target] = [program]

    @classmethod
    def checked(cls, command):
        result = subprocess.run(command, cwd=cls.work, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"{shlex.join(map(str, command))}\n{result.stdout}{result.stderr}")
        return result

    @classmethod
    def generate(cls, target, split="none", label=None, spec=None, extra=()):
        root = cls.work / (label or target)
        root.mkdir()
        generated = root / ("tool.py" if target == "python3" else "tool.c")
        cls.checked([DRIVER, "--mode=interpret", "-T", target, "--split", split,
                     "--out-dir", root, "-o", generated, *extra, spec or cls.spec])
        return generated

    def invoke(self, target, source, **kwargs):
        return subprocess.run(self.commands[target], cwd=self.work, input=source,
                              capture_output=True, text=True, timeout=10, **kwargs)

    def success(self, target, source):
        result = self.invoke(target, source)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "42")

    def failure(self, target, source, diagnostic):
        result = self.invoke(target, source)
        self.assertGreater(result.returncode, 0, f"unexpected exit status: {result.returncode}")
        self.assertIn(diagnostic, result.stderr)

    @unittest.skipUnless(os.name == "posix", "native descriptors require POSIX")
    def test_pipe_fifo_and_local_socket(self):
        for target in self.commands:
            with self.subTest(target=target):
                self.success(target, "pipe")
                self.success(target, "socketpair")
                fifo = self.work / f"{target}-fifo"
                self.success(target, f"fifo {fifo}")
                self.assertFalse(fifo.exists(), "FIFO directory entry was not removed")
                fifo.write_text("not a FIFO", encoding="utf-8")
                self.failure(target, f"fifo-unlink {fifo}", "path is not a FIFO")
                self.assertEqual(fifo.read_text(), "not a FIFO")
                link = fifo.with_suffix(".link")
                link.symlink_to(fifo)
                self.failure(target, f"fifo-read {link}", "path is not a FIFO")

    @unittest.skipUnless(os.name == "posix", "FIFOs require POSIX")
    def test_named_fifo_with_host_peer(self):
        for target in self.commands:
            with self.subTest(target=target):
                fifo = self.work / f"{target}-peer-fifo"
                self.success(target, f"fifo-create {fifo}")
                with self.assertRaises(FileExistsError):
                    os.mkfifo(fifo)
                anchor = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                try:
                    writer = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                    try:
                        os.write(writer, b"*\x00\xff")
                        self.success(target, f"fifo-read {fifo}")
                    finally:
                        os.close(writer)
                finally:
                    os.close(anchor)
                    self.success(target, f"fifo-unlink {fifo}")

    @unittest.skipUnless(os.name == "posix", "descriptor inheritance requires POSIX")
    def test_real_inherited_descriptor(self):
        for target in self.commands:
            with self.subTest(target=target):
                r, w = os.pipe()
                try:
                    os.write(w, b"0\x00\xff")
                    os.close(w)
                    w = None
                    p = self.invoke(target, f"read-inherited {r}", pass_fds=(r,))
                    self.assertEqual(p.returncode, 0, p.stderr)
                    self.assertEqual(p.stdout.strip(), "42")
                finally:
                    os.close(r)
                    if w is not None:
                        os.close(w)

    @unittest.skipUnless(os.name == "posix", "native descriptors require POSIX")
    def test_runtime_errors(self):
        for target in self.commands:
            with self.subTest(target=target):
                self.failure(target, "would-block", "IPC_WOULD_BLOCK: read:")
                self.failure(target, "broken-pipe", "IPC: write:")
                self.failure(target, "invalid-bytes", "expected an octet vector")
                self.failure(target, "invalid-descriptor", "IPC: close:")
                self.failure(target, "invalid-size", "read size")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux POSIX message queues")
    def test_real_message_queues_across_processes(self):
        for target in self.commands:
            with self.subTest(target=target):
                name = f"/llsynth-mq-{uuid4().hex}"
                produced = self.invoke(target, f"mq-produce {name}")
                try:
                    if "IPC_UNAVAILABLE" in produced.stderr:
                        self.skipTest(produced.stderr.strip())
                    self.assertEqual(produced.returncode, 0, produced.stderr)
                    self.success(target, f"mq-consume {name}")
                    self.failure(target, f"mq-empty {name}", "IPC_WOULD_BLOCK: mqReceive:")
                    self.success(target, f"mq-unlink {name}")
                    self.failure(target, f"mq-consume {name}", "IPC: mqOpen:")
                finally:
                    self.invoke(target, f"mq-unlink {name}")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux /dev/shm")
    def test_real_shared_memory_across_processes(self):
        for target in self.commands:
            with self.subTest(target=target):
                name = f"/llsynth-shm-{uuid4().hex}"
                produced = self.invoke(target, f"shm-produce {name}")
                try:
                    if "IPC_UNAVAILABLE" in produced.stderr:
                        self.skipTest(produced.stderr.strip())
                    self.assertEqual(produced.returncode, 0, produced.stderr)
                    self.assertEqual((Path("/dev/shm") / name[1:]).read_bytes(), b"\x00\x00\x00\xff*\x00\x00\x00")
                    self.success(target, f"shm-consume {name}")
                    self.failure(target, f"shm-produce {name}", "IPC: shmCreate:")
                    self.failure(target, f"shm-out-of-bounds {name}", "would extend the shared memory object")
                    self.success(target, f"shm-unlink {name}")
                    self.failure(target, f"shm-consume {name}", "IPC: shmOpen:")
                finally:
                    self.invoke(target, f"shm-unlink {name}")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux POSIX named semaphores")
    def test_real_named_semaphores_across_processes(self):
        backends = list(self.commands)
        for index, target in enumerate(backends):
            with self.subTest(target=target):
                peer = backends[(index + 1) % len(backends)]
                name = f"/llsynth-sem-{uuid4().hex}"
                created = self.invoke(target, f"sem-create {name}")
                try:
                    if "IPC_UNAVAILABLE" in created.stderr:
                        self.skipTest(created.stderr.strip())
                    self.assertEqual(created.returncode, 0, created.stderr)
                    self.assertEqual(created.stdout.strip(), "42")
                    self.assertEqual(self.invoke(target, "supports-sem").stdout.strip(), "1")
                    self.failure(target, f"sem-create {name}", "IPC: semCreate:")
                    self.failure(target, f"sem-stale {name}", "invalid or closed semaphore handle")
                    self.failure(target, "sem-invalid", "invalid or closed semaphore handle")
                    self.failure(target, f"sem-negative {name}", "initial semaphore value")
                    self.success(peer, f"sem-produce {name}")
                    self.success(target, f"sem-consume {name}")
                    self.success(peer, f"sem-hold-unlink {name}")
                    self.failure(target, f"sem-produce {name}", "IPC: semOpen:")
                    self.success(target, f"sem-create {name}")
                    self.success(peer, f"sem-unlink {name}")
                finally:
                    self.invoke(target, f"sem-unlink {name}")

    def test_python_unavailable_host(self):
        generated = self.commands["python3"][-1]
        loader = importlib.util.spec_from_file_location("ipc_generated", generated)
        module = importlib.util.module_from_spec(loader)
        with patch.dict(sys.modules, {loader.name: module}):
            loader.loader.exec_module(module)
            with patch.object(module, "_ipc_os", types.SimpleNamespace(name="nt")):
                self.assertEqual(module.Context().interpret("supports-pipe"), 0)
                self.assertEqual(module.Context().interpret("supports-sem"), 0)
                with self.assertRaisesRegex(module.LanguageError, "IPC_UNAVAILABLE: pipe:"):
                    module.Context().interpret("pipe")
            with patch.object(module, "_ipc_sys", types.SimpleNamespace(platform="darwin")):
                self.assertEqual(module.Context().interpret("supports-sem"), 0)
                with self.assertRaisesRegex(module.LanguageError, "IPC_UNAVAILABLE: semOpen:"):
                    module.Context().interpret("sem-produce /unavailable")

    @unittest.skipUnless(HAS_CC, "C compiler unavailable")
    def test_disabled_native_build(self):
        path = self.generate("c11", label="disabled")
        binary = path.with_suffix("")
        self.checked([*CC, "-std=c11", "-pedantic-errors", "-DLLS_IPC_DISABLE", path, "-lm", "-o", binary])
        for text, status, pattern in (("supports-pipe", 0, "0"), ("pipe", 1, "IPC_UNAVAILABLE: pipe:"),
                                      ("supports-sem", 0, "0"),
                                      ("sem-produce /unavailable", 1, "IPC_UNAVAILABLE: semOpen:")):
            p = subprocess.run([binary], cwd=self.work, input=text, text=True, capture_output=True, timeout=5)
            self.assertEqual(p.returncode, status, p.stderr)
            self.assertIn(pattern, p.stdout + p.stderr)

    def test_import_and_type_check(self):
        self.checked([DRIVER, "--check", "--mode=interpret", self.spec])
        for target in self.commands:
            with self.subTest(target=target):
                deps = self.work / f"{target}.d"
                suffix = "py" if target == "python3" else "c"
                self.generate(target, label=f"{target}-deps", extra=("--depfile", deps))
                text = deps.read_text(encoding="utf-8")
                self.assertIn("/stdlib/ipc.llsynth", text)
                self.assertIn(f"/stdlib/ipc/runtime.{suffix}", text)
        bad = self.work / "bad.llsynth"
        bad.write_text(SPEC.replace("IPC:write(w, [0, 255, 3])", 'IPC:write(w, "wrong")'),
                       encoding="utf-8")
        result = subprocess.run([DRIVER, "--check", "--mode=interpret", bad], cwd=self.work,
                                capture_output=True, text=True, timeout=20)
        self.assertGreater(result.returncode, 0)
        self.assertIn("type mismatch", result.stderr)

    def test_no_runtime_without_import(self):
        path = self.work / "no-import.llsynth"
        path.write_text(".rule Start\n'syntax: EOF\n'semantics: return 42;\n.end\n", encoding="utf-8")
        generated = self.generate("python3", label="no-import-python", spec=path)
        self.assertNotIn("def _ipc_execute", generated.read_text(encoding="utf-8"))
        if HAS_CC:
            generated = self.generate("c11", label="no-import-c", spec=path)
            self.assertNotIn("lls_ipc_call", generated.read_text(encoding="utf-8"))
            self.checked([*CC, "-std=c11", generated, "-lm", "-o", generated.with_suffix("")])


if __name__ == "__main__":
    unittest.main()
