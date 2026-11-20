"""Exercise the supported Python API and a clean installed C++ consumer."""

from pathlib import Path
import shutil
import struct
import subprocess
import sys
import sysconfig
import tempfile
import unittest

from archon import api
from archon.irgen import api as irgen


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec/riscv/rv32i.arch"
RV64IM = ROOT / "stdspec/riscv/rv64im.arch"
WORK = Path("/tmp/opencode")


def run_command(test, *args, cwd):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=180)
    test.assertEqual(result.returncode, 0, f"{args!r}\n{result.stdout}\n{result.stderr}")


class PublicPythonAPI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = api.compile_file(RV32, features=["I"])

    def test_database_generators_and_exports(self):
        api.validate_source(RV32, parameters={"XLEN": 32}, features=["I"])
        with self.assertRaisesRegex(api.ArchonError, "feature"):
            api.validate_source(RV32, features=["missing"])
        self.assertEqual(api.parse_source("architecture Test {}")["name"], "Test")
        with tempfile.TemporaryDirectory(dir=WORK if WORK.is_dir() else None) as work:
            filename = Path(work) / "arch.archdb"
            api.save_database(self.db, filename)
            snapshot = api.inspect_database(api.load_database(filename))
            self.assertEqual(snapshot["architecture"], "RISCV32")
            self.assertEqual(snapshot["address_width"], 32)
            self.assertIn("ADD", {row["name"] for row in snapshot["instructions"]})
            self.assertEqual(api.parse_database(api.dump_database(snapshot)), self.db)
            snapshot["features"].clear()
            self.assertEqual(self.db["features"], ["I"])
            snapshot["version"] = 99
            with self.assertRaisesRegex(api.ArchonError, "version"):
                api.validate_database(snapshot)
        self.assertIn("demo_encode", api.generate_codec(self.db, prefix="demo"))
        self.assertIn("demo_emu_state", api.generate_emulator(self.db, prefix="demo"))
        self.assertIn("demo_emu_state", api.generate_emulator_header(self.db, prefix="demo"))
        self.assertIn("compile_file", api.exports())
        self.assertIs(api.get_export("Target"), api.Target)
        self.assertIsNone(api.dispatch("validate_database", self.db))
        with self.assertRaisesRegex(api.ArchonError, "unknown Archon API export"):
            api.dispatch("__dict__")

    def test_assembly_and_execution(self):
        target = api.Target(self.db)
        name, operands = target.assemble("add x1, x2, x3")
        self.assertEqual((name, operands), ("ADD", {"rd": 1, "rs1": 2, "rs2": 3}))
        encoded = target.encode(name, operands)
        self.assertEqual(encoded, bytes.fromhex("b3003100"))
        self.assertEqual(target.decode(encoded), (name, operands))
        self.assertEqual(target.disassemble(encoded), "add x1, x2, x3")
        program = target.assemble_program("start: add x1, x2, x3\naddi x4, x1, @start\n",
                                          base_address=1024)
        self.assertEqual(target.disassemble_program(program, base_address=1024),
                         "add x1, x2, x3\naddi x4, x1, -4\n")
        initial = target.new_state()
        initial.registers[2:4] = [5, 7]
        self.assertIsInstance(initial, api.MachineState)
        self.assertEqual(target.execute_state(name, operands, initial).registers[1], 12)
        self.assertEqual(initial.registers[1], 0)
        self.assertEqual(target.execute_program_state(program, initial).registers[4], 8)
        with self.assertRaisesRegex(api.ArchonError, "example.asm:1:1"):
            target.assemble_program("unrecognized", filename="example.asm")

    def test_irgen_stages_macros_writer_and_native_bridges(self):
        self.assertIn("gated", irgen.available_representations())
        self.assertEqual(irgen.get_representation("gssa")["name"], "gated")
        self.assertIn(RV32, irgen.standard_spec_entry_points())
        self.assertIn("proof-tool", irgen.pipeline_stages())
        options = {"oracle": False}
        context = irgen.build_context("gssa", [self.db], options=options)
        self.assertEqual(context["architecture_names"], ["RISCV32"])
        self.assertNotEqual(id(context["architectures"][0]), id(self.db))
        proof = irgen.emit_stage("proof-tool", context)
        self.assertIn("; archon-rule ", proof["Proofs.smt"])
        with self.assertRaisesRegex(api.ArchonError, "unknown IRGen stage"):
            irgen.emit_stage("macros", context)
        out = irgen.generate_ir("gssa", [self.db], options=options)
        self.assertEqual(out["Proofs.smt"], proof["Proofs.smt"])
        self.assertIn("Translate.py", out)
        self.assertNotIn("Oracle.c", out)
        empty = irgen.dispatch("macros.emit_empty_artifact", "mir")
        stream = irgen.dispatch("macros.parse_stream", empty)
        self.assertEqual(stream.artifact, irgen.macros.ArtifactKind.MIR)
        self.assertIn("solve_smt", irgen.exports())
        self.assertEqual(irgen.dispatch("solve_smt", "(assert false)\n(check-sat)")[0], "unsat")
        with self.assertRaisesRegex(api.ArchonError, "not callable"):
            irgen.dispatch("EXPECTED_REPRESENTATIONS")
        with self.assertRaisesRegex(api.ArchonError, "unknown Archon IRGen API export"):
            irgen.get_export("core._invoke")
        self.assertEqual(irgen.resolve_native_target(api.compile_file(RV64IM))["abi"], "LP64")
        with tempfile.TemporaryDirectory(dir=WORK if WORK.is_dir() else None) as work:
            dest = Path(work) / "artifacts"
            self.assertEqual(irgen.write_generated_directory(out, dest), dest.resolve())
            self.assertEqual((dest / "Proofs.smt").read_text(), proof["Proofs.smt"])
            protected = dest / "IR.h"
            with self.assertRaisesRegex(api.ArchonError, "would overwrite input"):
                irgen.write_generated_directory(proof, dest, input_paths=[protected], force=True)
            # Invalid bridge requests must fail before launching any tool.
            with self.assertRaisesRegex(api.ArchonError, "unknown native assembler flavor"):
                irgen.assemble_object("missing", "missing", "invalid", "input.mir", dest / "a.o")
            with self.assertRaisesRegex(api.ArchonError, "entry symbol"):
                irgen.link_executable("missing", [], "--help", dest / "a.elf")
            self.assertFalse((dest / "a.o").exists())
            self.assertFalse((dest / "a.elf").exists())

    def test_native_elf_bridge_success(self):
        cmake, cc, clang, lld = map(shutil.which, ("cmake", "cc", "clang", "ld.lld"))
        if not all((cmake, cc, clang, lld)):
            self.skipTest("CMake, a C compiler, Clang, and LLD are required")
        db = api.compile_file(RV64IM)
        outputs = irgen.generate_ir("gated", [db], options={"oracle": False, "profile": "native"})
        with tempfile.TemporaryDirectory(dir=WORK if WORK.is_dir() else None) as temporary:
            work = Path(temporary)
            generated = irgen.write_generated_directory(outputs, work / "generated")
            build = work / "build"
            run_command(self, cmake, "-S", str(generated), "-B", str(build), cwd=work)
            run_command(self, cmake, "--build", str(build), "--target", "archon-backend",
                        "--parallel", "2", cwd=work)
            mir, obj, exe = (work / name for name in ("entry.mir", "entry.o", "entry.elf"))
            mir.write_text(
                "archon-mir 1\nmodule 1 0 0\nfunction _start i64 0 1 1 0\n"
                "types i64\nblock 0 1\nparams\nconst 0 42\nret 0\nend\n",
                encoding="utf-8",
            )
            irgen.assemble_object(build / "archon-backend", clang, "clang", mir, obj)
            irgen.link_executable(lld, [obj], "_start", exe)
            for file, kind in ((obj, 1), (exe, 2)):
                data = file.read_bytes()
                self.assertEqual(data[:6], b"\x7fELF\x02\x01")
                self.assertEqual(struct.unpack_from("<HH", data, 16), (kind, 243))


class InstalledCxxAPI(unittest.TestCase):
    def test_installed_consumer(self):
        if not all(map(shutil.which, ("cmake", "ctest", "c++"))):
            self.skipTest("CMake, CTest, and a C++ compiler are required")
        if not (Path(sysconfig.get_path("include")) / "Python.h").is_file():
            self.skipTest("CPython development headers are required")
        with tempfile.TemporaryDirectory(prefix="archon-embedding-", dir=WORK if WORK.is_dir() else None) as work:
            work = Path(work)
            host, install, consumer = work / "host", work / "install prefix", work / "consumer"
            options = ("-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_STANDARD=17",
                       "-DCMAKE_CXX_STANDARD_REQUIRED=ON")
            run_command(self, "cmake", "-S", str(ROOT), "-B", str(host),
                        f"-DPython3_EXECUTABLE={sys.executable}", *options, cwd=work)
            run_command(self, "cmake", "--build", str(host), "--config", "Release", cwd=work)
            run_command(self, "cmake", "--install", str(host), "--config", "Release",
                        "--prefix", str(install), cwd=work)
            run_command(self, "cmake", "-S", str(ROOT / "tests/embedding_consumer"), "-B", str(consumer),
                        f"-DCMAKE_PREFIX_PATH={install}", *options, cwd=work)
            run_command(self, "cmake", "--build", str(consumer), "--config", "Release", cwd=work)
            run_command(self, "ctest", "--test-dir", str(consumer), "-C", "Release",
                        "--output-on-failure", cwd=work)


if __name__ == "__main__":
    unittest.main()
