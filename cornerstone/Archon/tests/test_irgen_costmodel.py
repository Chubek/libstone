"""Compile and execute the generated multidimensional cost model."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen.costmodel import emit


ROOT = Path(__file__).resolve().parents[1]
STDSPEC = ROOT / "stdspec"


class GeneratedCostModel(unittest.TestCase):
    def compiler(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")
        return command

    def compile_and_run(self, outputs, source):
        command = self.compiler()
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            for name, text in outputs.items():
                (root / name).write_text(text, encoding="ascii")
            (root / "test.c").write_text(source, encoding="ascii")
            executable = root / "test"
            subprocess.run(
                [
                    *command,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "CostModel.c",
                    "test.c",
                    "-o",
                    executable,
                ],
                cwd=root,
                check=True,
            )
            subprocess.run([executable], cwd=root, check=True)

    def databases(self):
        return (
            compile_file(STDSPEC / "riscv" / "rv32i.arch"),
            compile_file(STDSPEC / "wasm" / "root.arch"),
        )

    def test_deterministic_database_driven_tables(self):
        physical, registerless = self.databases()
        context = {
            "architectures": [physical, registerless],
            "options": {"cost_profile": "O3"},
        }
        first = emit(context)
        second = emit(context)
        self.assertEqual(first, second)
        self.assertEqual(
            set(first),
            {
                "CostModel.c",
                "CostModel.h",
                "Cost_RISCV32.i",
                "Cost_Wasm32.i",
            },
        )
        for database in (physical, registerless):
            fragment = first[f"Cost_{database['architecture']}.i"]
            for instruction in database["instructions"]:
                self.assertIn(f'"{instruction["name"]}"', fragment)
        self.assertIn("ARCHON_COST_STATE_STACK_READ", first["Cost_Wasm32.i"])
        self.assertNotIn('ARCHON_COST_INSTRUCTION(\n    0u, 0u,\n    "Wasm32"', first["Cost_Wasm32.i"])

    def test_invalid_profile_is_rejected(self):
        physical, _ = self.databases()
        with self.assertRaises(ArchonError):
            emit(
                {
                    "architectures": [physical],
                    "options": {"cost_profile": "imaginary"},
                }
            )

    def test_strict_c99_physical_and_registerless_costs(self):
        physical, registerless = self.databases()
        outputs = emit(
            {
                "architectures": [physical, registerless],
                "options": {"cost_profile": "O3"},
            }
        )
        source = r"""
#include "CostModel.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static ArchonCost known_cost(
        uint64_t bytes, uint64_t instructions, uint64_t latency,
        uint64_t throughput, uint64_t pressure, uint64_t memory) {
    ArchonCost result = archon_cost_zero();
    result.code_bytes = archon_cost_value_known(bytes);
    result.instruction_count = archon_cost_value_known(instructions);
    result.latency = archon_cost_value_known(latency);
    result.throughput = archon_cost_value_known(throughput);
    result.register_pressure = archon_cost_value_known(pressure);
    result.memory_traffic = archon_cost_value_known(memory);
    return result;
}

int main(void) {
    const ArchonInstructionCost *add;
    const ArchonInstructionCost *drop;
    const ArchonInstructionCost *f32_add;
    const ArchonCostProfile *profile;
    ArchonCost sequence[2];
    ArchonCost aggregate;
    ArchonCost left;
    ArchonCost right;
    ArchonCostRelation relation;
    ArchonCostProfile weighted;
    ArchonCostProfile lexicographic;
    ArchonCostDimension order[2] = {
        ARCHON_COST_LATENCY, ARCHON_COST_CODE_BYTES
    };
    double weights[ARCHON_COST_DIMENSION_COUNT] = {
        0.5, 0.0, 0.5, 0.0, 0.0, 0.0
    };
    double normalizers[ARCHON_COST_DIMENSION_COUNT] = {
        10.0, 1.0, 10.0, 1.0, 1.0, 1.0
    };
    double score;
    size_t selected;
    size_t index;
    int saw_operand_stack = 0;

    assert(archon_cost_architecture_count() == 2u);
    assert(strcmp(archon_cost_architecture(0u)->name, "RISCV32") == 0);
    assert(strcmp(archon_cost_architecture(1u)->name, "Wasm32") == 0);
    assert(archon_cost_instruction_count() > 100u);
    for (index = 0u; index < archon_cost_instruction_count(); ++index) {
        const ArchonInstructionCost *row = archon_cost_instruction(index);
        assert(row != NULL);
        assert(row->cost.code_bytes.known);
        assert(row->cost.instruction_count.known &&
               row->cost.instruction_count.value == 1u);
        assert(!row->cost.latency.known);
        assert(!row->cost.throughput.known);
        assert(!row->cost.register_pressure.known);
        assert(!row->cost.memory_traffic.known);
    }

    add = archon_cost_find_instruction("RISCV32", "ADD");
    drop = archon_cost_instruction_by_name("Wasm32", "DROP");
    f32_add = archon_cost_find_instruction("Wasm32", "F32_ADD");
    assert(add != NULL && drop != NULL && f32_add != NULL);
    assert(add->cost.code_bytes.known && add->cost.code_bytes.value == 4u);
    assert(drop->cost.code_bytes.known && drop->cost.code_bytes.value == 1u);
    assert(add->cost.instruction_count.known &&
           add->cost.instruction_count.value == 1u);
    assert(!add->cost.latency.known);
    assert(!add->cost.throughput.known);
    assert(!add->cost.register_pressure.known);
    assert(!add->cost.memory_traffic.known);
    assert(!drop->cost.memory_traffic.known);
    assert((add->effect_flags & ARCHON_COST_EFFECT_REGISTER_READ) != 0u);
    assert((add->effect_flags & ARCHON_COST_EFFECT_REGISTER_WRITE) != 0u);
    assert((drop->effect_flags & ARCHON_COST_EFFECT_STACK_READ) != 0u);
    assert((drop->effect_flags & ARCHON_COST_EFFECT_STACK_WRITE) != 0u);
    assert((f32_add->effect_flags & ARCHON_COST_EFFECT_UNKNOWN) != 0u);
    assert(!f32_add->cost.memory_traffic.known);
    for (index = 0u; index < drop->effect_count; ++index) {
        const ArchonCostStateEffect *effect =
            archon_cost_instruction_effect(drop, index);
        assert(effect != NULL && effect->instruction_index == drop->index);
        if (strcmp(effect->resource, "OperandStack") == 0)
            saw_operand_stack = 1;
    }
    assert(saw_operand_stack);

    sequence[0] = add->cost;
    sequence[1] = drop->cost;
    assert(archon_cost_sequence(sequence, 2u, &aggregate) == ARCHON_COST_OK);
    assert(aggregate.code_bytes.known && aggregate.code_bytes.value == 5u);
    assert(aggregate.instruction_count.known &&
           aggregate.instruction_count.value == 2u);
    assert(!aggregate.latency.known);
    assert(!aggregate.throughput.known);
    assert(!aggregate.register_pressure.known);
    assert(!aggregate.memory_traffic.known);

    sequence[0] = archon_cost_zero();
    sequence[1] = archon_cost_zero();
    sequence[0].latency = archon_cost_value_unknown();
    sequence[1].latency = archon_cost_value_known(7u);
    sequence[0].register_pressure = archon_cost_value_known(3u);
    sequence[1].register_pressure = archon_cost_value_known(8u);
    assert(archon_cost_aggregate_sequence(sequence, 2u, &aggregate) ==
           ARCHON_COST_OK);
    assert(!aggregate.latency.known);
    assert(aggregate.register_pressure.known &&
           aggregate.register_pressure.value == 8u);

    sequence[0] = archon_cost_zero();
    sequence[1] = archon_cost_zero();
    sequence[0].code_bytes = archon_cost_value_known(UINT64_MAX);
    sequence[1].code_bytes = archon_cost_value_known(1u);
    assert(archon_cost_sequence(sequence, 2u, &aggregate) ==
           ARCHON_COST_OVERFLOW);
    assert(!aggregate.code_bytes.known);
    assert(aggregate.instruction_count.known &&
           aggregate.instruction_count.value == 0u);

    left = known_cost(4u, 1u, 2u, 1u, 2u, 0u);
    right = known_cost(8u, 2u, 3u, 2u, 4u, 8u);
    assert(archon_cost_pareto_compare(&left, &right) == ARCHON_COST_BETTER);
    left.latency = archon_cost_value_known(9u);
    right.latency = archon_cost_value_known(3u);
    assert(archon_cost_pareto_compare(&left, &right) ==
           ARCHON_COST_INCOMPARABLE);
    left = known_cost(4u, 1u, 2u, 1u, 2u, 0u);
    right = left;
    right.throughput = archon_cost_value_unknown();
    assert(archon_cost_pareto_mask(
               &left, &right, ARCHON_COST_ALL_DIMENSIONS, &relation) ==
           ARCHON_COST_UNAVAILABLE);
    assert(relation == ARCHON_COST_RELATION_UNKNOWN);

    assert(archon_cost_profile_init_weighted(
               &weighted, "test-weighted", weights, normalizers) ==
           ARCHON_COST_OK);
    left = known_cost(4u, 1u, 6u, 1u, 1u, 0u);
    right = known_cost(8u, 1u, 4u, 1u, 1u, 0u);
    assert(archon_cost_score(&left, &weighted, &score) == ARCHON_COST_OK);
    assert(score > 0.499999 && score < 0.500001);
    assert(archon_cost_compare(&left, &right, &weighted, &relation) ==
           ARCHON_COST_OK);
    assert(relation == ARCHON_COST_BETTER);
    sequence[0] = right;
    sequence[1] = left;
    assert(archon_cost_select_best(sequence, 2u, &weighted, &selected) ==
           ARCHON_COST_OK);
    assert(selected == 1u);
    sequence[0] = left;
    sequence[1] = left;
    assert(archon_cost_select_best(sequence, 2u, &weighted, &selected) ==
           ARCHON_COST_OK);
    assert(selected == 0u);

    assert(archon_cost_profile_init_lexicographic(
               &lexicographic, "test-lex", order, 2u) == ARCHON_COST_OK);
    assert(archon_cost_compare(&left, &right, &lexicographic, &relation) ==
           ARCHON_COST_OK);
    assert(relation == ARCHON_COST_WORSE);

    profile = archon_cost_default_profile();
    assert(profile != NULL && strcmp(profile->name, "O3") == 0);
    assert(archon_cost_select_profile("Oz") ==
           archon_cost_profile_by_name("Oz"));
    assert(archon_cost_select_profile("missing") == NULL);
    assert(archon_cost_compare(
               &add->cost, &drop->cost,
               archon_cost_profile_by_name("size"), &relation) ==
           ARCHON_COST_OK);
    assert(relation == ARCHON_COST_WORSE);
    assert(archon_cost_score(
               &add->cost, archon_cost_profile_by_name("Oz"), &score) ==
           ARCHON_COST_OK);
    assert(archon_cost_compare(
               &add->cost, &drop->cost, profile, &relation) ==
           ARCHON_COST_UNAVAILABLE);
    return 0;
}
"""
        self.compile_and_run(outputs, source)

    def test_empty_selection_is_strict_c99(self):
        outputs = emit({"architectures": [], "options": {}})
        self.assertEqual(set(outputs), {"CostModel.c", "CostModel.h"})
        self.compile_and_run(
            outputs,
            r"""
#include "CostModel.h"
#include <assert.h>
int main(void) {
    assert(archon_cost_architecture_count() == 0u);
    assert(archon_cost_instruction_count() == 0u);
    assert(archon_cost_state_effect_count() == 0u);
    assert(archon_cost_default_profile() != 0);
    return 0;
}
""",
        )


if __name__ == "__main__":
    unittest.main()
