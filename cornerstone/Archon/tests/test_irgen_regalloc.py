"""Strict-C99 coverage for the untyped TAC and linear-scan generators."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest

from archon.irgen import macros
from archon.irgen import oracle
from archon.irgen.regalloc import emit
from archon.irgen.untyped_tac import ALIASES, NAME, emit_ir


class IRGenRegalloc(unittest.TestCase):
    def compiler(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        return cc

    def compile(self, cc, root, source, output, *extra):
        subprocess.run(
            [
                *cc,
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                *extra,
                source,
                "-o",
                output,
            ],
            cwd=root,
            check=True,
        )

    def write_files(self, root, files):
        for name, text in files.items():
            (root / name).write_text(text, encoding="ascii")

    def test_untyped_tac_program_macro_builder_verifier_and_printer(self):
        self.assertEqual(NAME, "untyped-tac")
        self.assertIn("utac", ALIASES)
        cc = self.compiler()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, emit_ir(None))
            (root / "Program.i").write_text(
                """\
IR_PARAMETER(0, 0)
IR_CONSTANT(1, 7)
IR_BINARY(2, IR_OPERATOR_ADD, 0, 1)
IR_DECLARE_LABEL(0)
IR_BRANCH(2, 0, 1)
IR_LABEL(0)
IR_CALL1(3, "print", 2)
IR_COPY(4, 3)
IR_CALL_VOID1("sink", 4)
IR_RETURN(4)
IR_LABEL(1)
IR_EFFECT(IR_EFFECT_TRAP, 2, "nonzero")
IR_RETURN_VOID()
""",
                encoding="ascii",
            )
            (root / "test.c").write_text(
                """\
#include "IR.h"
#include <assert.h>
#include <string.h>

int main(void) {
    struct IR_program program;
    struct IR_diagnostic diagnostic;
    FILE *output;
    char text[512];
    size_t size;
    assert(IR_load_program(&program, &diagnostic) == IR_OK);
    assert(program.temporary_count == 5);
    output = tmpfile();
    assert(output != NULL);
    assert(IR_print(output, &program, &diagnostic) == IR_OK);
    assert(fseek(output, 0, SEEK_END) == 0);
    size = (size_t)ftell(output);
    assert(size < sizeof text);
    rewind(output);
    assert(fread(text, 1, size, output) == size);
    text[size] = '\\0';
    assert(strstr(text, "%t2 = add %t0, %t1") != NULL);
    assert(strstr(text, "branch %t2, L0, L1") != NULL);
    assert(strstr(text, "call print(%t2)") != NULL);
    assert(strstr(text, "%t4 = copy %t3") != NULL);
    assert(strstr(text, "call sink(%t4)") != NULL);
    assert(strstr(text, "effect.trap nonzero %t2") != NULL);
    assert(fclose(output) == 0);
    IR_program_destroy(&program);
    return 0;
}
""",
                encoding="ascii",
            )
            self.compile(
                cc,
                root,
                "IR.c",
                "test",
                '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                "test.c",
            )
            subprocess.run([str(root / "test")], cwd=root, check=True)

    def test_holes_fixed_aliases_spill_reload_and_stability(self):
        cc = self.compiler()
        db = {
            "registers": {
                "count": 4,
                "prefix": "r",
                "zero": 0,
                "aliases": {"zero": 0, "fixed": 2},
            }
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root, emit({"db": db, "options": {"oracle": False}})
            )
            (root / "Scheduled.i").write_text(
                "/* Explicit ranges make scheduling positions self-contained. */\n",
                encoding="ascii",
            )
            (root / "MIR.i").write_text(
                """\
MIR_RANGE(1, 0, 2)
MIR_RANGE(1, 8, 10)
MIR_RANGE(2, 2, 8)
MIR_RANGE(3, 0, 10)
MIR_RANGE(4, 3, 9)
MIR_DEF(3, 4)
MIR_USE(8, 4)
""",
                encoding="ascii",
            )
            (root / "RegMap.i").write_text(
                """\
REGMAP_ALIAS_GROUP(3, 2)
REGMAP_ALIAS("fixed_alias", 3)
REGMAP_FIXED(3, "fixed_alias")
""",
                encoding="ascii",
            )
            self.compile(cc, root, "Regalloc.c", "regalloc")
            command = [
                str(root / "regalloc"),
                "Scheduled.i",
                "MIR.i",
                "RegMap.i",
                "Regallocated.i",
            ]
            subprocess.run(command, cwd=root, check=True)
            first = (root / "Regallocated.i").read_text(encoding="ascii")
            subprocess.run(command, cwd=root, check=True)
            self.assertEqual(
                first, (root / "Regallocated.i").read_text(encoding="ascii")
            )
            canonical = macros.parse_stream(first, "regallocated")
            self.assertGreaterEqual(len(canonical.records), 1)
            self.assertIn('#include "MacroABI.h"', first)
            # Value 1 is inactive while value 2 is live, so both use r1.
            self.assertIn("REGALLOC_ASSIGN(1, 1)", first)
            self.assertIn("REGALLOC_ASSIGN(2, 1)", first)
            self.assertIn("REGALLOC_FIXED(3, 2)", first)
            self.assertIn("REGALLOC_ASSIGN(3, 2)", first)
            self.assertIn("REGALLOC_ALIAS_GROUP(3, 2)", first)
            self.assertIn('REGALLOC_ALIAS("zero", 0)', first)
            self.assertIn("REGALLOC_ZERO(0)", first)
            self.assertNotIn("REGALLOC_ASSIGN(1, 0)", first)
            self.assertIn("REGALLOC_SPILL_SLOT(4, 0)", first)
            self.assertIn("REGALLOC_SPILL(3, 4, 0)", first)
            self.assertIn("REGALLOC_RELOAD(8, 4, 0)", first)
            self.assertIn("REGALLOC_ABSTRACT_SPILL(4, 0)", first)

    def test_canonical_macro_abi_inputs_preserve_64_bit_ids(self):
        cc = self.compiler()
        names = (
            "mir",
            "block",
            "vreg",
            "op0",
            "op1",
            "instr-map",
            "map0",
            "map1",
            "scheduled",
            "region",
            "scheduled0",
            "scheduled1",
            "reg-map",
            "register0",
            "register1",
            "alias",
            "reserved",
            "interval",
            "event0",
            "event1",
            "fixed",
        )
        ids = {name: macros.stable_id("regalloc.fixture", name) for name in names}
        position = 1 << 40
        self.assertTrue(all(value > 0xFFFFFFFF for value in ids.values()))

        mir = macros.emit_mir(
            [
                macros.Record("BLOCK", ids["block"], 0, "entry", 0),
                macros.Record("VREG", ids["vreg"], 0, "GPR", 32, 0),
                macros.Record(
                    "INSTRUCTION",
                    ids["op0"],
                    ids["block"],
                    0,
                    "DEF",
                    1,
                    0,
                    0,
                    0,
                    0,
                    0,
                ),
                macros.Record("DEF", ids["op0"], 0, ids["vreg"], ""),
                macros.Record(
                    "INSTRUCTION",
                    ids["op1"],
                    ids["block"],
                    1,
                    "USE",
                    0,
                    1,
                    0,
                    0,
                    0,
                    0,
                ),
                macros.Record("USE", ids["op1"], 0, ids["vreg"], ""),
            ],
            stream_id=ids["mir"],
        )
        scheduled = macros.emit_scheduled(
            [
                macros.Record(
                    "REGION",
                    ids["region"],
                    0,
                    ids["block"],
                    1,
                    "ARCHON_SCHEDULE_FALLBACK",
                    0,
                ),
                macros.Record(
                    "INSTRUCTION",
                    ids["scheduled0"],
                    0,
                    ids["map0"],
                    ids["region"],
                    ids["block"],
                    position,
                    0,
                    0,
                ),
                macros.Record(
                    "INSTRUCTION",
                    ids["scheduled1"],
                    1,
                    ids["map1"],
                    ids["region"],
                    ids["block"],
                    position + 1,
                    0,
                    0,
                ),
            ],
            stream_id=ids["scheduled"],
            mir_stream_id=ids["mir"],
            instr_map_stream_id=ids["instr-map"],
        )
        reg_map = macros.emit_reg_map(
            [
                macros.Record(
                    "REGISTER", ids["register0"], 0, "r0", "GPR", 32, 0
                ),
                macros.Record(
                    "REGISTER", ids["register1"], 1, "r1", "GPR", 32, 0
                ),
                macros.Record(
                    "ALIAS",
                    ids["alias"],
                    0,
                    "zero",
                    ids["register0"],
                    0,
                    32,
                    "ARCHON_WRITE_DISCARD",
                ),
                macros.Record(
                    "RESERVED",
                    ids["reserved"],
                    0,
                    ids["register0"],
                    "hardwired zero",
                    0,
                ),
                macros.Record("VREG", ids["vreg"], 0, "GPR", 32, 0),
                macros.Record(
                    "INTERVAL",
                    ids["interval"],
                    0,
                    ids["vreg"],
                    position,
                    position + 2,
                    0,
                ),
                macros.Record(
                    "EVENT",
                    ids["event0"],
                    0,
                    ids["scheduled0"],
                    position,
                    "ARCHON_REG_EVENT_DEF",
                    ids["vreg"],
                    0,
                ),
                macros.Record(
                    "EVENT",
                    ids["event1"],
                    1,
                    ids["scheduled1"],
                    position + 1,
                    "ARCHON_REG_EVENT_USE",
                    ids["vreg"],
                    0,
                ),
                macros.Record(
                    "FIXED",
                    ids["fixed"],
                    0,
                    ids["vreg"],
                    ids["register1"],
                    0,
                ),
            ],
            stream_id=ids["reg-map"],
            target="fixture",
            scheduled_stream_id=ids["scheduled"],
            mir_stream_id=ids["mir"],
        )

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root,
                emit({
                    "db": {"registers": None},
                    "options": {"oracle": False},
                }),
            )
            self.write_files(
                root,
                {
                    "MacroABI.h": macros.macro_abi_header(),
                    "MIR.i": mir,
                    "Scheduled.i": scheduled,
                    "RegMap.i": reg_map,
                },
            )
            self.compile(cc, root, "Regalloc.c", "regalloc")
            subprocess.run([str(root / "regalloc")], cwd=root, check=True)

            output = (root / "Regallocated.i").read_text(encoding="ascii")
            parsed = macros.parse_stream(output, "regallocated")
            self.assertEqual(
                parsed.metadata["scheduled_stream_id"], ids["scheduled"]
            )
            self.assertEqual(parsed.metadata["reg_map_stream_id"], ids["reg-map"])
            assignments = [
                record
                for record in parsed.records
                if record.macro == "ARCHON_REGALLOCATED_ASSIGN"
            ]
            events = [
                record
                for record in parsed.records
                if record.macro == "ARCHON_REGALLOCATED_EVENT"
            ]
            self.assertEqual(len(assignments), 1)
            self.assertEqual(assignments[0].args[2], ids["vreg"])
            self.assertEqual(assignments[0].args[4], ids["register1"])
            self.assertEqual(assignments[0].args[6], 32)
            self.assertEqual(
                [record.args[2] for record in events],
                [ids["scheduled0"], ids["scheduled1"]],
            )

            (root / "include.c").write_text(
                '#include "Regallocated.i"\nint main(void) { return 0; }\n',
                encoding="ascii",
            )
            self.compile(cc, root, "include.c", "include")

    def test_no_register_target_spills_without_inventing_an_opcode(self):
        cc = self.compiler()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root,
                emit({
                    "db": {"registers": None},
                    "options": {"oracle": False},
                }),
            )
            (root / "Scheduled.i").write_text("", encoding="ascii")
            (root / "MIR.i").write_text(
                "MIR_DEF(0, 9)\nMIR_USE(1, 9)\n", encoding="ascii"
            )
            (root / "RegMap.i").write_text("", encoding="ascii")
            self.compile(cc, root, "Regalloc.c", "regalloc")
            subprocess.run([str(root / "regalloc")], cwd=root, check=True)
            result = (root / "Regallocated.i").read_text(encoding="ascii")
            self.assertIn("REGALLOC_BEGIN(1, 1)", result)
            self.assertIn("REGALLOC_SPILL_SLOT(9, 0)", result)
            self.assertIn("REGALLOC_SPILL(0, 9, 0)", result)
            self.assertIn("REGALLOC_RELOAD(1, 9, 0)", result)
            self.assertNotIn("REGALLOC_ASSIGN(9,", result)
            self.assertNotIn("LOAD_OPCODE", result)
            self.assertNotIn("STORE_OPCODE", result)

    def test_conflicting_fixed_intervals_report_a_diagnostic(self):
        cc = self.compiler()
        db = {
            "registers": {
                "count": 2,
                "prefix": "r",
                "zero": 0,
                "aliases": {},
            }
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root, emit({"db": db, "options": {"oracle": False}})
            )
            (root / "Scheduled.i").write_text("", encoding="ascii")
            (root / "MIR.i").write_text(
                "MIR_RANGE(1, 0, 4)\nMIR_RANGE(2, 2, 5)\n", encoding="ascii"
            )
            (root / "RegMap.i").write_text(
                "REGMAP_FIXED(1, 1)\nREGMAP_FIXED(2, 1)\n", encoding="ascii"
            )
            self.compile(cc, root, "Regalloc.c", "regalloc")
            completed = subprocess.run(
                [str(root / "regalloc")],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("conflicting fixed intervals", completed.stderr)

    def test_oracle_annotations_conservatively_influence_allocation(self):
        cc = self.compiler()
        context = {
            "db": {
                "registers": {
                    "count": 4,
                    "prefix": "r",
                    "zero": 0,
                    "aliases": {},
                }
            },
            "options": {"oracle": True},
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, oracle.emit(context))
            generated = emit(context)
            self.assertIn('#include "Oracle.h"', generated["Regalloc.c"])
            self.assertEqual(
                generated["Regalloc.c"].count(
                    "archon_oracle_process_file("
                ),
                1,
            )
            run_files = generated["Regalloc.c"].split(
                "enum RA_status RA_run_files(", 1
            )[1]
            self.assertLess(
                run_files.index("RA_process_oracle_artifact("),
                run_files.index("RA_open_and_load("),
            )
            self.assertLess(
                run_files.index("RA_open_and_load("),
                run_files.index("RA_apply_oracle_annotations("),
            )
            self.write_files(root, generated)
            (root / "Scheduled.i").write_text("", encoding="ascii")
            (root / "MIR.i").write_text(
                """\
MIR_RANGE(1, 0, 10)
MIR_RANGE(2, 1, 5)
MIR_RANGE(3, 2, 4)
""",
                encoding="ascii",
            )
            (root / "RegMap.i").write_text("", encoding="ascii")
            (root / "worker.c").write_text(
                """\
#include "Oracle.h"
#include <string.h>

static ArchonOracleStatus transform(
        const ArchonOracleWorkerRequest *request,
        ArchonOracleWorkerResponse *response,
        void *user_data,
        ArchonOracleDiagnostic *diagnostic) {
    static const char replacement[] = "REGMAP_FIXED(1, 2)\\n";
    static const char malformed[] = "REGMAP_FIXED(\\n";
    const char *artifact =
        user_data == NULL ? replacement : malformed;
    size_t artifact_size = strlen(artifact);
    ArchonOracleStatus status;
    (void)diagnostic;
    if (request->artifact_capacity < artifact_size)
        return ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED;
    memcpy(request->artifact_data, artifact, artifact_size);
    response->artifact_size = artifact_size;
    response->artifact_changed = 1;
    if (user_data != NULL)
        return ARCHON_ORACLE_STATUS_OK;
    status = archon_oracle_response_add_bool(
        response, "regalloc.reserved_register.3", 1);
    if (status == ARCHON_ORACLE_STATUS_OK)
        status = archon_oracle_response_add_u64(
            response, "regalloc.spill_cost.2", UINT64_C(100));
    if (status == ARCHON_ORACLE_STATUS_OK)
        status = archon_oracle_response_add_u64(
            response, "regalloc.spill_cost.3", UINT64_C(1));
    if (status == ARCHON_ORACLE_STATUS_OK)
        status = archon_oracle_response_add_string(
            response, "regalloc.raw_code", "REGALLOC_ASSIGN(3, 3)");
    return status;
}

int main(int argc, char **argv) {
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    if (argc != 2 && argc != 3)
        return 2;
    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, argv[1], 1u, 5000u, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_OK)
        status = archon_oracle_server_serve_once(
            &server, transform, argc == 3 ? argv[2] : NULL, &diagnostic);
    archon_oracle_server_dispose(&server);
    return status == ARCHON_ORACLE_STATUS_OK ? 0 : 1;
}
""",
                encoding="ascii",
            )
            self.compile(
                cc, root, "Regalloc.c", "regalloc", "Oracle.c"
            )
            self.compile(
                cc, root, "Oracle.c", "worker", "worker.c"
            )
            socket_path = root / "oracle.sock"
            worker = subprocess.Popen(
                [str(root / "worker"), str(socket_path)],
                cwd=root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            for _ in range(500):
                if socket_path.exists() or worker.poll() is not None:
                    break
                time.sleep(0.01)
            self.assertTrue(socket_path.exists())
            environment = os.environ.copy()
            environment["ARCHON_ORACLE_SOCKET"] = str(socket_path)
            subprocess.run(
                [str(root / "regalloc")],
                cwd=root,
                env=environment,
                check=True,
            )
            stdout, stderr = worker.communicate(timeout=10)
            self.assertEqual(worker.returncode, 0, stdout + stderr)
            result = (root / "Regallocated.i").read_text(encoding="ascii")
            self.assertIn("REGALLOC_FIXED(1, 2)", result)
            self.assertIn("REGALLOC_ASSIGN(1, 2)", result)
            self.assertIn("REGALLOC_RESERVED(3)", result)
            self.assertIn("REGALLOC_ASSIGN(2, 1)", result)
            self.assertIn("REGALLOC_SPILL_SLOT(3, 0)", result)
            self.assertNotIn("regalloc.raw_code", result)

            (root / "Regallocated.i").unlink()
            (root / "RegMap.i").write_text("", encoding="ascii")
            (root / "RegMap.oracle.i").unlink()
            malformed_socket = root / "malformed.sock"
            worker = subprocess.Popen(
                [
                    str(root / "worker"),
                    str(malformed_socket),
                    "malformed",
                ],
                cwd=root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            for _ in range(500):
                if malformed_socket.exists() or worker.poll() is not None:
                    break
                time.sleep(0.01)
            self.assertTrue(malformed_socket.exists())
            environment["ARCHON_ORACLE_SOCKET"] = str(malformed_socket)
            malformed = subprocess.run(
                [str(root / "regalloc")],
                cwd=root,
                env=environment,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            stdout, stderr = worker.communicate(timeout=10)
            self.assertEqual(worker.returncode, 0, stdout + stderr)
            self.assertNotEqual(malformed.returncode, 0)
            self.assertIn("parse error", malformed.stderr)
            self.assertEqual(
                (root / "RegMap.i").read_text(encoding="ascii"),
                "REGMAP_FIXED(\n",
            )
            self.assertFalse((root / "Regallocated.i").exists())

    def test_oracle_malformed_illegal_and_unknown_annotations(self):
        cc = self.compiler()
        context = {
            "db": {
                "registers": {
                    "count": 2,
                    "prefix": "r",
                    "zero": 0,
                    "aliases": {},
                }
            },
            "options": {"oracle": True},
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, oracle.emit(context))
            self.write_files(root, emit(context))
            (root / "Scheduled.i").write_text("", encoding="ascii")
            (root / "MIR.i").write_text(
                "MIR_RANGE(1, 0, 2)\n", encoding="ascii"
            )
            (root / "RegMap.i").write_text("", encoding="ascii")
            self.compile(
                cc, root, "Regalloc.c", "regalloc", "Oracle.c"
            )

            sidecar = root / "RegMap.oracle.i"
            sidecar.write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 5)
ARCHON_ORACLE_U64("regalloc.spill_cost.1", UINT64_C(1)
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            malformed = subprocess.run(
                [str(root / "regalloc")],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertNotEqual(malformed.returncode, 0)
            self.assertIn("cannot load Oracle annotations", malformed.stderr)
            self.assertFalse((root / "Regallocated.i").exists())

            sidecar.write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 5)
ARCHON_ORACLE_U64("regalloc.spill_cost.1", UINT64_C(1000001))
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            illegal = subprocess.run(
                [str(root / "regalloc")],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertNotEqual(illegal.returncode, 0)
            self.assertIn("bounded U64", illegal.stderr)
            self.assertFalse((root / "Regallocated.i").exists())

            sidecar.write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 5)
ARCHON_ORACLE_STRING("regalloc.raw_code", "REGALLOC_ASSIGN(1, 0)")
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            subprocess.run([str(root / "regalloc")], cwd=root, check=True)
            result = (root / "Regallocated.i").read_text(encoding="ascii")
            self.assertIn("REGALLOC_ASSIGN(1, 1)", result)
            self.assertNotIn("REGALLOC_ASSIGN(1, 0)", result)

    def test_oracle_false_has_no_compile_or_link_dependency(self):
        files = emit({
            "db": {"registers": None},
            "options": {"oracle": False},
        })
        self.assertNotIn("Oracle.h", files["Regalloc.c"])
        self.assertNotIn("archon_oracle_", files["Regalloc.c"])


if __name__ == "__main__":
    unittest.main()
