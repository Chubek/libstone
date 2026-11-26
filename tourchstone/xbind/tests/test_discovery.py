#!/usr/bin/env python3
"""Exercise offline XBind inventory and matching, never native-load fixtures."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


class Discovery(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="discovery-", dir=ARGS.work)
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)

    def run_ok(self, *args):
        result = subprocess.run(args, cwd=self.work, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def cli_bad(self, diagnostic, *args):
        result = subprocess.run([ARGS.cli, *args], cwd=self.work, capture_output=True,
                                text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertIn(diagnostic, result.stderr)

    def spec(self, name="probe", options=""):
        p = self.work / "fixture.xbind"
        p.write_text('''local x = require("lxbind")
return x.library{name="fixture",headers={"fixture.h"},targets={x.target("Lua")},
  functions={x.func("%s",{returns="int",params={x.param("n","int")}%s})}}
''' % (name, options), encoding="utf-8")
        (self.work / "fixture.h").write_text(f'extern "C" int {name}(int);\n', encoding="utf-8")
        return p

    def check_image(self, path, fmt, arch, export, spec, with_arch=False):
        arch_arg = [arch] if with_arch else []
        text = self.run_ok(ARGS.cli, "--discover", path, *arch_arg)
        self.assertEqual(text, self.run_ok(ARGS.cli, "--discover", path, *arch_arg))
        inventory = json.loads(text)
        self.assertEqual(inventory["image"]["format"].lower(), fmt)
        self.assertEqual(inventory["image"]["arch"], arch)
        self.assertEqual(inventory["image"]["kind"], "shared-library")
        self.assertIn(export, {s["name"] for s in inventory["exports"]})
        self.assertNotIn("signature", text)
        text = self.run_ok(ARGS.cli, "--bind-ir", path, spec, *arch_arg)
        self.assertEqual(text, self.run_ok(ARGS.cli, "--bind-ir", path, spec, *arch_arg))
        artifact = json.loads(text)
        self.assertEqual(artifact["signature_status"], "declared-unchecked")
        self.assertEqual(artifact["bindings"][0]["export"]["name"], export)
        self.assertEqual(artifact["manifest"]["functions"][0]["params"][0]["c"], "int")
        return inventory, artifact

    def test_capabilities(self):
        caps = json.loads(self.run_ok(ARGS.cli, "--capabilities"))
        self.assertEqual(caps["binary_discovery"], bool(ARGS.enabled))
        self.assertEqual(len(caps["targets"]), 9)
        self.assertEqual([t["name"] for t in caps["targets"] if t["backend"] != "scaffold-only"], ["Lua"])
        if not ARGS.enabled:
            self.cli_bad("not built", "--discover", "unused")

    def require_tools(self, *names):
        if not ARGS.enabled:
            self.skipTest("qBFD discovery not enabled")
        missing = [name for name in names if not shutil.which(name)]
        if missing:
            self.skipTest("missing cross tools: " + ", ".join(missing))

    def build_object(self, target, obj, *flags):
        (self.work / "fixture.c").write_text("int probe(int n) { return n + 2; }\n",
                                             encoding="utf-8")
        self.run_ok("clang", "--target=" + target, *flags, "-c", "fixture.c", "-o", obj)

    def test_elf_versioned_and_rejections(self):
        self.require_tools("clang", "ld.lld")
        src = self.work / "elf.c"
        src.write_text('''int current(int n) { return n + 7; }
int old(int n) { return n + 3; }
__asm__(".symver current,probe@@NEW");
__asm__(".symver old,probe@OLD");
''', encoding="utf-8")
        (self.work / "exports.map").write_text("OLD { }; NEW { local: *; } OLD;\n", encoding="utf-8")
        self.run_ok("clang", "--target=x86_64-linux-gnu", "-fPIC", "-c", src, "-o", "elf.o")
        self.run_ok("ld.lld", "-shared", "--version-script=exports.map", "elf.o", "-o", "elf.so")
        spec = self.spec()
        _, artifact = self.check_image("elf.so", "elf", "x86-64", "probe", spec, True)
        self.assertEqual(artifact["bindings"][0]["export"]["version"], "NEW")
        check = self.work / "check.cpp"
        check.write_text(self.run_ok(ARGS.cli, "--emit-check", spec), encoding="utf-8")
        self.run_ok(ARGS.cxx, "-std=c++20", "-fsyntax-only", check)
        alias = self.spec("api_probe", ',symbol="probe",symbol_version="OLD"')
        _, artifact = self.check_image("elf.so", "elf", "x86-64", "probe", alias)
        self.assertEqual(artifact["bindings"][0]["export"]["version"], "OLD")
        self.assertEqual(artifact["bindings"][0]["declaration"], "api_probe")
        self.cli_bad("does not support binary symbol overrides", "--emit-lua", alias)
        self.cli_bad("no matching export", "--bind-ir", "elf.so", self.spec("absent"))
        self.cli_bad("requires a shared-library image", "--bind-ir", "elf.o", self.spec())
        self.cli_bad("does not match", "--discover", "elf.so", "aarch64")
        self.cli_bad("unknown architecture", "--discover", "elf.so", "made-up")
        self.cli_bad("requires IMAGE SPEC", "--bind-ir", "elf.so")

    def test_pe_exports(self):
        self.require_tools("clang", "lld-link")
        self.build_object("x86_64-pc-windows-msvc", "fixture.obj")
        self.run_ok("lld-link", "/dll", "/noentry", "/export:probe", "/out:fixture.dll", "fixture.obj")
        inventory, artifact = self.check_image("fixture.dll", "pe", "x86-64", "probe", self.spec())
        self.assertEqual(inventory["image"]["abi"], "win64")
        self.assertIsInstance(artifact["bindings"][0]["export"]["ordinal"], int)
        self.cli_bad("symbol_version requires ELF", "--bind-ir", "fixture.dll",
                     self.spec(options=',symbol_version="X"'))

    def test_macho_thin_and_universal(self):
        self.require_tools("clang", "ld64.lld")
        slices = []
        for arch, cpu, subtype in (("x86_64", 0x1000007, 3), ("arm64", 0x100000c, 0)):
            self.build_object(arch + "-apple-macosx11.0", arch + ".o")
            image = arch + ".dylib"
            self.run_ok("ld64.lld", "-dylib", "-arch", arch, "-platform_version", "macos",
                        "11.0", "11.0", "-o", image, arch + ".o")
            slices.append((cpu, subtype, (self.work / image).read_bytes()))
        self.check_image("x86_64.dylib", "mach-o", "x86-64", "_probe", self.spec())
        info, _ = self.check_image("arm64.dylib", "mach-o", "aarch64", "_probe", self.spec())
        self.assertEqual(info["image"]["abi"], "darwin_arm64")

        # Deterministic two-slice fixture; all offsets are aligned to 16 KiB.
        alignment = 1 << 14
        data = bytearray(struct.pack(">II", 0xcafebabe, len(slices)))
        offset = alignment
        for cpu, subtype, payload in slices:
            data.extend(struct.pack(">IIIII", cpu, subtype, offset, len(payload), 14))
            offset += (len(payload) + alignment - 1) // alignment * alignment
        for _, _, payload in slices:
            data.extend(b"\0" * (-len(data) % alignment))
            data.extend(payload)
        (self.work / "fat.dylib").write_bytes(data)
        self.cli_bad("requires an ARCH", "--discover", "fat.dylib")
        self.check_image("fat.dylib", "mach-o", "aarch64", "_probe", self.spec(), True)
        self.check_image("fat.dylib", "mach-o", "x86-64", "_probe", self.spec(), True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--cxx", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--enabled", type=int, choices=(0, 1), required=True)
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
