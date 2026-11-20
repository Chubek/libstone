"""Tests for the ArchDB-to-IR target-facts boundary."""

from copy import deepcopy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen.core import (
    compile_standard_architectures,
    standard_spec_entry_points,
)
from archon.irgen.targetfacts import emit, normalize


ROOT = Path(__file__).resolve().parents[1]
STDSPEC = ROOT / "stdspec"
TEMP_ROOT = Path("/tmp/opencode")


def _macro_count(name, filename, index):
    return f"""
enum {{
    target_macro_{name.lower()}_{index} = 0
#define ARCHON_TARGET_{name}(...) + 1
#include "{filename}"
#undef ARCHON_TARGET_{name}
}};
"""


class TargetFactsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = standard_spec_entry_points()
        cls.databases = compile_standard_architectures()

    def test_every_standard_spec_compiles_and_runs_as_strict_c99(self):
        self.assertEqual(len(self.entries), len(self.databases))
        self.assertGreater(len(self.databases), 0)
        facts = normalize({"architectures": self.databases})
        generated = emit({"architectures": self.databases})
        macro_files = [
            f"Target_{architecture['identifier']}.i" for architecture in facts
        ]
        self.assertEqual(
            set(macro_files),
            set(generated) - {"TargetFacts.h", "TargetFacts.c"},
        )

        TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(
            prefix="archon-targetfacts-", dir=TEMP_ROOT
        ) as temporary:
            directory = Path(temporary)
            for name, text in generated.items():
                (directory / name).write_text(text, encoding="utf-8", newline="\n")

            macro_checks = []
            for index, (architecture, filename) in enumerate(zip(facts, macro_files)):
                macro_checks.append(_macro_count("INSTRUCTION", filename, index))
                macro_checks.append(_macro_count("REGISTER", filename, index))
                macro_checks.append(_macro_count("OPERAND_REGISTER_RANGE", filename, index))
            # C99 has no static_assert. Keep the macro includes at file scope
            # and test their enumerator values at runtime.
            macro_source = "\n".join(macro_checks)
            runtime_macro_assertions = []
            for index, architecture in enumerate(facts):
                runtime_macro_assertions.extend(
                    [
                        f"    assert(target_macro_instruction_{index} == "
                        f"{len(architecture['instructions'])});",
                        f"    assert(target_macro_register_{index} == "
                        f"{len(architecture['registers'])});",
                        f"    assert(target_macro_operand_register_range_{index} == "
                        f"{sum(op['register_range_explicit'] for inst in architecture['instructions'] for op in inst['operands'])});",
                    ]
                )

            expected_instructions = sum(
                len(architecture["instructions"]) for architecture in facts
            )
            runner = f"""#include "TargetFacts.h"
#include <assert.h>
#include <stddef.h>
{macro_source}
static const int expects_abi[] = {{
    {", ".join(str(int(arch["abi_known"])) for arch in facts)}
}};
int main(void) {{
    size_t i;
    assert(archon_target_facts_validate());
    assert(archon_target_architecture_count == {len(facts)}u);
    assert(archon_target_instruction_count == {expected_instructions}u);
{os.linesep.join(runtime_macro_assertions)}
    for (i = 0u; i < archon_target_architecture_count; ++i) {{
        const ArchonTargetArchitectureFact *target =
            archon_target_architecture(i);
        assert(target != NULL);
        assert(archon_target_find_architecture(target->identifier) == target);
        assert(target->abi_knowledge ==
            (expects_abi[i] ? ARCHON_TARGET_KNOWN : ARCHON_TARGET_UNKNOWN));
        assert((target->abi_json != NULL) == expects_abi[i]);
    }}
    for (i = 0u; i < archon_target_instruction_count; ++i) {{
        const ArchonTargetInstructionFact *instruction =
            archon_target_instruction(i);
        const ArchonTargetArchitectureFact *target =
            archon_target_architecture(instruction->target_index);
        const ArchonTargetCostFact *code_size;
        const ArchonTargetScheduleFact *schedule;
        size_t j;
        assert(instruction != NULL);
        assert(instruction->size_bytes == instruction->encoding_width / 8u);
        assert(instruction->semantic_equivalence_knowledge ==
               ARCHON_TARGET_UNKNOWN);
        assert(instruction->cost_count == 5u);
        code_size = &archon_target_costs[instruction->first_cost];
        assert(code_size->kind == ARCHON_TARGET_COST_CODE_SIZE_BYTES);
        assert(code_size->knowledge == ARCHON_TARGET_KNOWN);
        assert(code_size->numerator == instruction->size_bytes);
        for (j = 1u; j < instruction->cost_count; ++j)
            assert(archon_target_costs[instruction->first_cost + j].knowledge ==
                   ARCHON_TARGET_UNKNOWN);
        schedule = &archon_target_schedules[instruction->schedule_index];
        assert(schedule->policy ==
               ARCHON_TARGET_SCHEDULE_CONSERVATIVE_UNKNOWN);
        assert(schedule->latency_knowledge == ARCHON_TARGET_UNKNOWN);
        assert(schedule->execution_resources_knowledge ==
               ARCHON_TARGET_UNKNOWN);
        assert(schedule->conservative_barrier);
        assert(!schedule->may_reorder && !schedule->may_speculate);
        for (j = 0u; j < instruction->operand_count; ++j) {{
            size_t child = instruction->first_operand + j;
            const ArchonTargetOperandFact *operand = &archon_target_operands[child];
            const ArchonTargetOperandClassFact *facts =
                &archon_target_operand_classes[operand->operand_class_index];
            uint32_t first = UINT32_MAX, last = UINT32_MAX;
            if (facts->kind == ARCHON_TARGET_OPERAND_REGISTER) {{
                assert(archon_target_operand_register_range(child, &first, &last));
                assert(first <= last && last < target->register_count);
                if (target->register_count == 16u && facts->encoding_width == 3u)
                    assert(first == 0u && last == 7u);
            }} else {{
                assert(!archon_target_operand_register_range(child, &first, &last));
                assert(first == UINT32_MAX && last == UINT32_MAX);
            }}
        }}
    }}
    return 0;
}}
"""
            (directory / "runner.c").write_text(
                runner, encoding="ascii", newline="\n"
            )

            compilers = [
                compiler for compiler in ("gcc", "clang")
                if shutil.which(compiler)
            ]
            if not compilers:
                self.skipTest("neither GCC nor Clang is available")
            for compiler in compilers:
                with self.subTest(compiler=compiler):
                    executable = directory / f"targetfacts-{compiler}"
                    subprocess.run(
                        [
                            compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-pedantic", str(directory / "TargetFacts.c"),
                            str(directory / "runner.c"), "-I", str(directory),
                            "-o", str(executable),
                        ],
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                    subprocess.run(
                        [str(executable)],
                        check=True,
                        capture_output=True,
                        text=True,
                    )

    def test_registers_state_effects_and_registerless_targets(self):
        by_name = {
            architecture["name"]: architecture
            for architecture in normalize({"architectures": self.databases})
        }
        riscv = by_name["RISCV32"]
        self.assertTrue(riscv["register_file"])
        self.assertEqual(riscv["register_count"], 32)
        self.assertTrue(riscv["zero_present"])
        self.assertEqual(riscv["zero_index"], 0)
        aliases = {row["name"]: row["register"] for row in riscv["aliases"]}
        self.assertEqual(aliases["sp"], 2)
        self.assertEqual(aliases["a0"], 10)
        add = next(row for row in riscv["instructions"] if row["name"] == "ADD")
        self.assertTrue(all(op["register_range"] == (0, 31) for op in add["operands"]))

        for name, expected in (("AArch64", "AAPCS64"), ("AMD64", "SysVAMD64")):
            with self.subTest(target=name):
                target = by_name[name]
                self.assertTrue(target["abi_known"])
                facts = json.loads(target["abi_json"])
                self.assertEqual((facts["schema_version"], facts["name"]), (2, expected))
        amd64 = by_name["AMD64"]
        self.assertEqual(amd64["register_count"], 16)
        mov = next(row for row in amd64["instructions"] if row["name"] == "MOV")
        self.assertTrue(all(op["register_range"] == (0, 7) for op in mov["operands"]))

        pdp11 = by_name["PDP11"]
        resources = {(row["kind"], row["name"]) for row in pdp11["resources"]}
        self.assertIn(("scalar", "C"), resources)
        self.assertIn(("memory", "MainMemory"), resources)
        adc = next(row for row in pdp11["instructions"] if row["name"] == "ADC")
        effects = {(row["kind"], row["resource"]) for row in adc["effects"]}
        self.assertIn(("read-register", "dst"), effects)
        self.assertIn(("read-scalar", "C"), effects)
        self.assertIn(("write-scalar", "N"), effects)

        for name in ("Wasm32", "JVM", "CIL"):
            with self.subTest(target=name):
                target = by_name[name]
                self.assertFalse(target["register_file"])
                self.assertEqual(target["register_count"], 0)
                self.assertEqual(target["register_width"], 0)
                self.assertFalse(target["zero_present"])
                self.assertEqual(target["registers"], [])
                self.assertTrue(
                    all(
                        operand_class["kind"] == "imm"
                        for operand_class in target["classes"]
                    )
                )

        wasm = by_name["Wasm32"]
        drop = next(row for row in wasm["instructions"] if row["name"] == "DROP")
        unreachable = next(
            row for row in wasm["instructions"] if row["name"] == "UNREACHABLE"
        )
        self.assertTrue(drop["semantics_known"])
        self.assertTrue(drop["effects_known"])
        self.assertFalse(unreachable["semantics_known"])
        self.assertFalse(unreachable["effects_known"])
        self.assertEqual(unreachable["effects"], [])

    def test_determinism_and_sanitized_collision_resistance(self):
        base = compile_file(STDSPEC / "riscv" / "rv32i.arch")
        first = deepcopy(base)
        first["architecture"] = "alpha-beta"
        second = deepcopy(base)
        second["architecture"] = "alpha_beta"
        configured = deepcopy(base)
        configured["architecture"] = "duplicate"
        configured["parameters"]["adapter_variant"] = 1
        other_configuration = deepcopy(configured)
        other_configuration["parameters"]["adapter_variant"] = 2
        exact_duplicate = deepcopy(configured)
        databases = [
            first, second, configured, other_configuration, exact_duplicate,
        ]
        before = json.dumps(databases, sort_keys=True)

        normalized = normalize({"architectures": databases})
        identifiers = [row["identifier"] for row in normalized]
        self.assertEqual(len(identifiers), len(set(map(str.casefold, identifiers))))
        self.assertNotEqual(identifiers[0], identifiers[1])
        self.assertEqual(normalized[2]["fingerprint"], normalized[4]["fingerprint"])
        self.assertNotEqual(identifiers[2], identifiers[4])
        for identifier in identifiers:
            self.assertRegex(identifier, r"^[A-Za-z_][A-Za-z0-9_]*$")

        outputs = emit({"architectures": databases})
        self.assertEqual(outputs, emit({"architectures": deepcopy(databases)}))
        self.assertEqual(before, json.dumps(databases, sort_keys=True))
        target_paths = sorted(
            name for name in outputs if name.startswith("Target_")
        )
        self.assertEqual(len(target_paths), len(databases))
        self.assertTrue(
            all(re.fullmatch(r"Target_[A-Za-z_][A-Za-z0-9_]*\.i", name)
                for name in target_paths)
        )

    def test_adapter_rejects_encoding_facts_the_database_does_not_cross_check(self):
        malformed = compile_file(STDSPEC / "riscv" / "rv32i.arch")
        malformed["instructions"][0]["fields"][0]["lsb"] = 32
        with self.assertRaisesRegex(ArchonError, "outside the instruction encoding"):
            normalize(malformed)
        with self.assertRaises(ArchonError):
            emit({"architectures": ["not-an-archdb"]})


if __name__ == "__main__":
    unittest.main()
