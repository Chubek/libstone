"""Strict-C99 integration tests for ANF Program.i lowering to MachineIR."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.irgen import anf, oracle, scheduler


PROGRAM = r'''IR_PROGRAM_BEGIN("dependency-test");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(left, "left", IR_TYPE_I32);
IR_PARAM(right, "right", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(sum, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
IR_ARG_VALUE(right);
IR_LET_END();
IR_EFFECT_BEGIN(IR_PRIM_STORE, IR_EFFECT_WRITE);
IR_ARG_SYMBOL("memory");
IR_ARG_VALUE(sum);
IR_EFFECT_END();
IR_LET_BEGIN(product, IR_TYPE_I32, IR_PRIM_MUL, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
IR_ARG_VALUE(right);
IR_LET_END();
IR_EFFECT_BEGIN(IR_PRIM_FENCE, IR_EFFECT_WRITE);
IR_EFFECT_END();
IR_RETURN(product);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
'''


DRIVER = r'''#include "Scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path) {
    FILE *input = fopen(path, "rb");
    char *text;
    long size;
    assert(input != NULL);
    assert(fseek(input, 0, SEEK_END) == 0);
    size = ftell(input);
    assert(size >= 0);
    assert(fseek(input, 0, SEEK_SET) == 0);
    text = (char *)malloc((size_t)size + 1u);
    assert(text != NULL);
    assert(fread(text, 1u, (size_t)size, input) == (size_t)size);
    text[size] = '\0';
    assert(fclose(input) == 0);
    return text;
}

static void lower(const char *path) {
    struct archon_ir_program program;
    struct archon_scheduler_options options;
    char diagnostic[256];
    FILE *output;
    assert(archon_ir_load_embedded(&program, diagnostic,
                                   sizeof diagnostic) == ARCHON_IR_OK);
    assert(program.function_count == 1u);
    assert(program.functions[0].values[2].type.kind == ARCHON_IR_T_I32);
    assert(archon_ir_verify(&program, diagnostic,
                            sizeof diagnostic) == ARCHON_IR_OK);
    archon_scheduler_default_options(&options);
    output = fopen(path, "wb");
    assert(output != NULL);
    assert(archon_scheduler_emit_mir(output, &program, &options,
               diagnostic, sizeof diagnostic) == ARCHON_SCHEDULER_OK);
    assert(fclose(output) == 0);
    archon_ir_program_dispose(&program);
}

static void reject_bad_types(void) {
    struct archon_ir_program program;
    struct archon_ir_builder builder;
    char diagnostic[256];
    archon_ir_program_init(&program);
    archon_ir_builder_init(&builder, &program);
    assert(archon_ir_builder_program_begin(&builder, "bad") == ARCHON_IR_OK);
    assert(archon_ir_builder_function_begin(
        &builder, "bad", "bad", IR_TYPE_I64) == ARCHON_IR_OK);
    assert(archon_ir_builder_parameter(
        &builder, "x", "x", IR_TYPE_I32) == ARCHON_IR_OK);
    assert(archon_ir_builder_block_begin(
        &builder, "entry", "entry") == ARCHON_IR_OK);
    assert(archon_ir_builder_let_begin(
        &builder, "wrong", "wrong", IR_TYPE_I64, IR_PRIM_ADD,
        IR_EFFECT_NONE) == ARCHON_IR_OK);
    assert(archon_ir_builder_argument_value(&builder, "x") == ARCHON_IR_OK);
    assert(archon_ir_builder_argument_value(&builder, "x") == ARCHON_IR_OK);
    assert(archon_ir_builder_operation_end(&builder) == ARCHON_IR_OK);
    assert(archon_ir_builder_return(&builder, "wrong") == ARCHON_IR_OK);
    assert(archon_ir_builder_block_end(&builder) == ARCHON_IR_OK);
    assert(archon_ir_builder_function_end(&builder) == ARCHON_IR_OK);
    assert(archon_ir_builder_program_end(&builder) == ARCHON_IR_OK);
    assert(archon_ir_verify(&program, diagnostic, sizeof diagnostic) ==
           ARCHON_IR_INVALID_PROGRAM);
    assert(strstr(diagnostic, "invalid types") != NULL);
    archon_ir_program_dispose(&program);
}

int main(void) {
    char *first, *second;
    reject_bad_types();
    lower("MIR.i");
    lower("MIR-again.i");
    first = read_file("MIR.i");
    second = read_file("MIR-again.i");
    assert(strcmp(first, second) == 0);

    assert(strstr(first, "MIR_SCHEMA(2)") != NULL);
    assert(strstr(first, "MIR_TARGET(0, \"Tiny32\", 32)") != NULL);
    assert(strstr(first,
        "MIR_TARGET_INSTRUCTION(0, 0, \"ADD\", 4, 1, "
        "0, 0, 0, 0, \"\", 0)") != NULL);
    /* ADD is retained as an encoding fact, but semantics=None does not make
       it a generic ADD candidate.  The exact ADD32 semantics does. */
    assert(strstr(first,
        "MIR_CANDIDATE_SET_BEGIN(0, MIR_OPCODE_ADD, 1)") != NULL);
    assert(strstr(first,
        "MIR_CANDIDATE(0, 0, 0, 1, \"Tiny32\", \"ADD32\", "
        "4, 1, 0, 0, 0, 0, \"\", 0)") != NULL);
    /* Encoding bytes are known; target timing/resource facts are explicit
       unknowns despite ad-hoc latency fields in the input object. */
    assert(strstr(first,
        "MIR_TARGET_INSTRUCTION(0, 1, \"ADD32\", 4, 1, "
        "0, 0, 0, 0, \"\", 0)") != NULL);
    assert(strstr(first, "\"COMPLEX_ADD\", 4, 1, 0, 0, 0, 0") != NULL);
    /* The store consumes the add result. */
    assert(strstr(first,
        "MIR_DEPENDENCY(1, 0, MIR_DEP_DATA)") != NULL);
    /* Effectful operations remain ordered even across a pure multiply. */
    assert(strstr(first,
        "MIR_DEPENDENCY(3, 1, MIR_DEP_EFFECT)") != NULL);
    /* The terminator records both data and control ordering explicitly. */
    assert(strstr(first,
        "MIR_DEPENDENCY(4, 2, MIR_DEP_DATA | MIR_DEP_CONTROL)") != NULL);
    assert(strstr(first,
        "MIR_DEPENDENCY(4, 3, MIR_DEP_EFFECT | MIR_DEP_CONTROL)") != NULL);
    assert(strstr(first,
        "MIR_OP(2, 0, 0, 2, 3, MIR_OPCODE_MUL, 2, "
        "MIR_EFFECT_NONE, 2)") != NULL);
    free(first);
    free(second);
    return 0;
}
'''


class IRGenScheduler(unittest.TestCase):
    def test_contract_and_documented_schemas(self):
        self.assertEqual(anf.NAME, "anf")
        self.assertIn("a-normal-form", anf.ALIASES)
        ir = anf.emit_ir(None)
        self.assertEqual(set(ir), {"IR.c", "IR.h"})
        self.assertIn("Program.i schema", ir["IR.h"])
        self.assertIn("archon_ir_verify", ir["IR.h"])
        generated = scheduler.emit({"options": {"oracle": False}})
        self.assertEqual(set(generated), {"Scheduler.c", "Scheduler.h"})
        self.assertIn("MachineIR (MIR.i) X-macro schema", generated["Scheduler.h"])
        self.assertIn("schema, version 2", generated["Scheduler.h"])
        self.assertIn("encoding_bytes_known", generated["Scheduler.h"])
        self.assertIn("latency_known", generated["Scheduler.h"])
        self.assertIn("target-independent unit weight", generated["Scheduler.h"])
        self.assertIn("MIR_DEPENDENCY", generated["Scheduler.h"])
        self.assertNotIn('"Oracle.h"', generated["Scheduler.c"])
        self.assertNotIn("@ORACLE_", generated["Scheduler.c"])
        self.assertNotIn("archon_oracle_", generated["Scheduler.c"])
        self.assertIn(
            '#include "Oracle.h"',
            scheduler.emit({})["Scheduler.c"],
        )
        enabled_source = scheduler.emit({})["Scheduler.c"]
        self.assertIn(
            "SCHEDULER_ORACLE_MAX_RECOGNIZED_FACTS = 4096",
            enabled_source,
        )
        self.assertNotIn(
            "archon_oracle_annotations_lookup_bool", enabled_source
        )

    def test_oracle_schedule_advice_is_parsed_once_and_only_adds_edges(self):
        program = r'''IR_PROGRAM_BEGIN("oracle-schedule");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(a, "a", IR_TYPE_I32);
IR_PARAM(b, "b", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(x, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(a);
IR_ARG_INT(IR_TYPE_I32, 1);
IR_LET_END();
IR_LET_BEGIN(y, IR_TYPE_I32, IR_PRIM_MUL, IR_EFFECT_NONE);
IR_ARG_VALUE(b);
IR_ARG_INT(IR_TYPE_I32, 2);
IR_LET_END();
IR_LET_BEGIN(z, IR_TYPE_I32, IR_PRIM_XOR, IR_EFFECT_NONE);
IR_ARG_VALUE(a);
IR_ARG_INT(IR_TYPE_I32, 3);
IR_LET_END();
IR_RETURN(x);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
'''
        advice = """\
ARCHON_ORACLE_BEGIN(1, 2)
ARCHON_ORACLE_U64("scheduler.operation.2.latency", UINT64_C(2))
ARCHON_ORACLE_I64("scheduler.operation.2.priority", INT64_C(100))
ARCHON_ORACLE_BOOL("scheduler.operation.1.depends_on.0", 1)
ARCHON_ORACLE_STRING("scheduler.operation.2.unknown", "preserved")
ARCHON_ORACLE_END()
"""
        driver = r'''#include "Scheduler.h"

#include <stdio.h>

int main(void) {
    struct archon_scheduler_options options;
    char diagnostic[256];
    enum archon_scheduler_status status;
    FILE *output;
    archon_scheduler_default_options(&options);
    options.annotation_path = "Advice.i";
    output = fopen("MIR.i", "wb");
    if (output == NULL)
        return 2;
    status = archon_scheduler_emit_embedded(
        output, &options, diagnostic, sizeof diagnostic);
    if (fclose(output) != 0 && status == ARCHON_SCHEDULER_OK)
        return 2;
    if (status != ARCHON_SCHEDULER_OK) {
        (void)fprintf(stderr, "%s\n", diagnostic);
        return 3;
    }
    return 0;
}
'''
        compilers = [
            compiler
            for compiler in ("gcc", "clang")
            if shutil.which(compiler)
        ]
        if not compilers:
            self.skipTest("no strict C99 compiler is available")
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-scheduler-oracle-"
        ) as raw:
            root = Path(raw)
            files = {
                **anf.emit_ir(None),
                **oracle.emit(None),
                **scheduler.emit({}),
            }
            for name, text in files.items():
                (root / name).write_text(text, encoding="ascii")
            (root / "Program.i").write_text(program, encoding="ascii")
            (root / "Advice.i").write_text(advice, encoding="ascii")
            (root / "driver.c").write_text(driver, encoding="ascii")
            for compiler in compilers:
                executable = root / f"driver-{compiler}"
                subprocess.run(
                    [
                        compiler,
                        "-std=c99",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pedantic",
                        '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                        "IR.c",
                        "Oracle.c",
                        "Scheduler.c",
                        "driver.c",
                        "-o",
                        executable.name,
                    ],
                    cwd=root,
                    check=True,
                )
                result = subprocess.run(
                    [str(executable)],
                    cwd=root,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                mir = (root / "MIR.i").read_text(encoding="ascii")
                self.assertLess(mir.index("MIR_OP(2,"), mir.index("MIR_OP(0,"))
                self.assertLess(mir.index("MIR_OP(0,"), mir.index("MIR_OP(1,"))
                self.assertIn(
                    "MIR_DEPENDENCY(1, 0, MIR_DEP_CONTROL)", mir
                )
                self.assertEqual(
                    (root / "Advice.i").read_text(encoding="ascii"),
                    advice,
                )

                (root / "Advice.i").write_text(
                    """\
ARCHON_ORACLE_BEGIN(1, 2)
ARCHON_ORACLE_BOOL("scheduler.operation.0.depends_on.1", 1)
ARCHON_ORACLE_END()
""",
                    encoding="ascii",
                )
                rejected = subprocess.run(
                    [str(executable)],
                    cwd=root,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(rejected.returncode, 3)
                self.assertIn(
                    "must name an earlier operation in the same block",
                    rejected.stderr,
                )
                (root / "Advice.i").write_text(advice, encoding="ascii")

    def test_program_to_mir_compiles_and_runs_with_strict_c99_compilers(self):
        bits32 = {"kind": "bits", "width": 32}

        def read(name):
            return {"op": "read", "name": name, "type": dict(bits32)}

        def local(name):
            return {"op": "local", "name": name, "type": dict(bits32)}

        def operands():
            return [
                {
                    "name": "dst",
                    "kind": "reg",
                    "direction": "out",
                    "width": 5,
                    "signed": False,
                },
                {
                    "name": "left",
                    "kind": "reg",
                    "direction": "in",
                    "width": 5,
                    "signed": False,
                },
                {
                    "name": "right",
                    "kind": "reg",
                    "direction": "in",
                    "width": 5,
                    "signed": False,
                },
            ]

        def exact_semantics(operation):
            return {
                "word_width": 32,
                "statements": [
                    {
                        "op": "let",
                        "name": "result",
                        "value": {
                            "op": operation,
                            "left": read("left"),
                            "right": read("right"),
                            "type": dict(bits32),
                        },
                    },
                    {"op": "write", "name": "dst", "value": local("result")},
                ],
                "effects": {"reads": ["left", "right"], "writes": ["dst"]},
            }

        complex_semantics = {
            "word_width": 32,
            "statements": [
                {
                    "op": "let",
                    "name": "sum",
                    "value": {
                        "op": "+%",
                        "left": read("left"),
                        "right": read("right"),
                        "type": dict(bits32),
                    },
                },
                {
                    "op": "write",
                    "name": "dst",
                    "value": {
                        "op": "~",
                        "value": local("sum"),
                        "type": dict(bits32),
                    },
                },
            ],
            "effects": {"reads": ["left", "right"], "writes": ["dst"]},
        }
        database = {
            "architecture": "Tiny32",
            "address_width": 32,
            "instructions": [
                {
                    # A mnemonic is not semantic evidence.
                    "name": "ADD",
                    "width": 32,
                    "operands": operands(),
                    "semantics": None,
                },
                {
                    "name": "ADD32",
                    "width": 32,
                    "operands": operands(),
                    "semantics": exact_semantics("+%"),
                    # These are deliberately outside validated ArchDB schema
                    # and must not become generated architecture facts.
                    "latency": 99,
                    "cost": {"code_size": 123},
                    "scheduling": {"latency": 77},
                },
                {
                    # Contains an add, but denotes NOT(ADD(...)), not ADD.
                    "name": "COMPLEX_ADD",
                    "width": 32,
                    "operands": operands(),
                    "semantics": complex_semantics,
                },
                {
                    "name": "MUL32",
                    "width": 32,
                    "operands": operands(),
                    "semantics": exact_semantics("*%"),
                },
            ],
        }
        tables = scheduler._tables({"architectures": [database]})
        target_facts, candidate_rows = tables[1], tables[2]
        self.assertIn('"ADD", 4u, 1u', target_facts)
        self.assertIn('"COMPLEX_ADD", 4u, 1u', target_facts)
        self.assertIn('"Tiny32", "ADD32", 4u, 1u', candidate_rows)
        self.assertNotIn('"Tiny32", "ADD",', candidate_rows)
        self.assertNotIn('"COMPLEX_ADD"', candidate_rows)
        self.assertNotIn("99u", candidate_rows)
        self.assertNotIn("123u", candidate_rows)
        self.assertNotIn("77u", candidate_rows)

        compilers = []
        requested = shlex.split(os.environ.get("CC", "cc"))
        if requested and shutil.which(requested[0]):
            compilers.append(requested)
        for name in ("gcc", "clang"):
            path = shutil.which(name)
            if path and all(command[0] != path for command in compilers):
                compilers.append([path])
        if not compilers:
            self.skipTest("no C99 compiler is available")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for filename, contents in anf.emit_ir(None).items():
                (root / filename).write_text(contents, encoding="ascii")
            for filename, contents in scheduler.emit(
                {
                    "architectures": [database],
                    "architecture_names": ["Tiny32"],
                    "options": {"oracle": False},
                }
            ).items():
                (root / filename).write_text(contents, encoding="ascii")
            (root / "Program.i").write_text(PROGRAM, encoding="ascii")
            (root / "driver.c").write_text(DRIVER, encoding="ascii")

            for index, compiler in enumerate(compilers):
                executable = f"driver-{index}"
                with self.subTest(compiler=compiler[0]):
                    subprocess.run(
                        [
                            *compiler,
                            "-std=c99",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-pedantic",
                            '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                            "IR.c",
                            "Scheduler.c",
                            "driver.c",
                            "-o",
                            executable,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run(
                        [str(root / executable)], cwd=root, check=True
                    )


if __name__ == "__main__":
    unittest.main()
