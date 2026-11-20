"""Compile and exercise the representation-neutral cross-stage verifier."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.irgen import verifier


_FIXTURE = r"""
#include "Verify.h"

#include <assert.h>
#include <stddef.h>

static int has_code(
    const struct archon_verify_report *report,
    enum archon_verify_code code) {
    size_t i;
    for (i = 0u; i < report->count; ++i)
        if (report->diagnostics[i].code == code)
            return 1;
    return 0;
}

static void *fail_allocate(void *user, size_t bytes) {
    (void)user;
    (void)bytes;
    return NULL;
}

static void fail_deallocate(void *user, void *pointer) {
    (void)user;
    (void)pointer;
}

int main(void) {
    struct archon_verify_block blocks[] = {
        {10u, 0u, 3u, 0u, 0u}
    };
    struct archon_verify_instruction instructions[] = {
        {100u, 10u, 1u, ARCHON_VERIFY_INST_EFFECTFUL},
        {101u, 10u, 2u, ARCHON_VERIFY_INST_EFFECTFUL},
        {102u, 10u, 3u, ARCHON_VERIFY_INST_TERMINATOR}
    };
    struct archon_verify_value values[] = {
        {200u, 100u, 5u, 0u}
    };
    struct archon_verify_use uses[] = {
        {250u, 101u, 200u, ARCHON_VERIFY_ID_NONE, 0u}
    };
    struct archon_verify_effect_dependency effect_dependencies[] = {
        {400u, 300u, ARCHON_VERIFY_ID_NONE}
    };
    struct archon_verify_effect effects[] = {
        {300u, 100u, 7u, 0u, 0u, ARCHON_VERIFY_EFFECT_ROOT},
        {301u, 101u, 7u, 0u, 1u, 0u}
    };
    struct archon_verify_ir program = {
        "fixture",
        ARCHON_VERIFY_IR_SSA |
            ARCHON_VERIFY_IR_REQUIRE_TERMINATORS |
            ARCHON_VERIFY_IR_REQUIRE_REACHABLE,
        10u,
        blocks, 1u,
        NULL, 0u,
        instructions, 3u,
        values, 1u,
        uses, 1u,
        effects, 2u,
        effect_dependencies, 1u
    };
    struct archon_verify_instruction_mapping mappings[] = {
        {500u, 100u, 0u, 1u, 0u},
        {501u, 101u, 1u, 1u, 0u},
        {502u, 102u, 2u, 1u, 0u}
    };
    struct archon_verify_mapping_target targets[] = {
        {510u, 100u}, {511u, 101u}, {512u, 102u}
    };
    struct archon_verify_instr_map instr_map = {
        &program, &program, mappings, 3u, targets, 3u
    };
    struct archon_verify_resource resources[] = {
        {50u, 1u}
    };
    struct archon_verify_reservation reservations[] = {
        {550u, 50u, 0u, 1u, 1u},
        {551u, 50u, 0u, 1u, 1u}
    };
    struct archon_verify_schedule_entry entries[] = {
        {600u, 100u, 10u, 0u, 0u, 0u, 1u},
        {601u, 101u, 10u, 1u, 0u, 1u, 1u},
        {602u, 102u, 10u, 2u, 0u, 2u, 0u}
    };
    struct archon_verify_schedule_dependency dependencies[] = {
        {650u, 100u, 101u, 1u, ARCHON_VERIFY_DEP_DATA}
    };
    struct archon_verify_scheduled scheduled = {
        &program, 1u,
        entries, 3u,
        dependencies, 1u,
        resources, 1u,
        reservations, 2u
    };
    struct archon_verify_virtual_register virtual_registers[] = {
        {700u, 5u, ARCHON_VERIFY_ID_NONE, 0u}
    };
    struct archon_verify_value_register value_registers[] = {
        {710u, 200u, 700u}
    };
    struct archon_verify_live_range live_ranges[] = {
        {720u, 700u, 10u, 0u, 2u}
    };
    struct archon_verify_reg_map reg_map = {
        &program,
        virtual_registers, 1u,
        value_registers, 1u,
        live_ranges, 1u
    };
    struct archon_verify_physical_register physical_registers[] = {
        {800u, 5u, UINT64_C(1), 0u},
        {801u, 5u, UINT64_C(2), ARCHON_VERIFY_PREG_RESERVED},
        {802u, 5u, UINT64_C(4), ARCHON_VERIFY_PREG_ZERO}
    };
    struct archon_verify_allocation allocations[] = {
        {810u, 700u, ARCHON_VERIFY_LOCATION_REGISTER, 800u, INT64_C(-1)}
    };
    struct archon_verify_regallocated regallocated = {
        &reg_map, physical_registers, 3u, allocations, 1u
    };
    struct archon_verify_pipeline pipeline = {
        &program, &program, &instr_map, &scheduled, &reg_map, &regallocated
    };
    struct archon_verify_diagnostic diagnostics[64];
    struct archon_verify_report report;
    struct archon_verify_config config;
    enum archon_verify_status status;

    archon_verify_config_init(&config);
    archon_verify_report_init(&report, diagnostics, 64u);
    assert(archon_verify_pipeline(&pipeline, &config, &report) ==
           ARCHON_VERIFY_OK);
    assert(report.count == 0u && report.total_count == 0u);
    assert(archon_verify_report_at(&report, 0u) == NULL);
    assert(archon_verify_status_name(ARCHON_VERIFY_OK) != NULL);
    assert(archon_verify_stage_name(ARCHON_VERIFY_STAGE_MIR) != NULL);
    assert(archon_verify_code_name(ARCHON_VERIFY_DIAG_INTERFERENCE) != NULL);
    assert(sizeof(archon_verify_id) == sizeof(uint64_t));

    /* A definition on only one arm of a diamond does not dominate merge. */
    {
        struct archon_verify_block diamond_blocks[] = {
            {1000u, 0u, 1u, 0u, 2u},
            {1001u, 1u, 1u, 2u, 1u},
            {1002u, 2u, 1u, 3u, 1u},
            {1003u, 3u, 1u, 4u, 0u}
        };
        struct archon_verify_edge diamond_edges[] = {
            {1100u, 1000u, 1001u}, {1101u, 1000u, 1002u},
            {1102u, 1001u, 1003u}, {1103u, 1002u, 1003u}
        };
        struct archon_verify_instruction diamond_instructions[] = {
            {1200u, 1000u, 1u, ARCHON_VERIFY_INST_TERMINATOR},
            {1201u, 1001u, 2u, ARCHON_VERIFY_INST_TERMINATOR},
            {1202u, 1002u, 3u, ARCHON_VERIFY_INST_TERMINATOR},
            {1203u, 1003u, 4u, ARCHON_VERIFY_INST_TERMINATOR}
        };
        struct archon_verify_value diamond_values[] = {
            {1300u, 1201u, ARCHON_VERIFY_ID_NONE, 0u}
        };
        struct archon_verify_use diamond_uses[] = {
            {1400u, 1203u, 1300u, ARCHON_VERIFY_ID_NONE, 0u}
        };
        struct archon_verify_ir diamond = {
            "diamond",
            ARCHON_VERIFY_IR_SSA |
                ARCHON_VERIFY_IR_REQUIRE_TERMINATORS |
                ARCHON_VERIFY_IR_REQUIRE_REACHABLE,
            1000u,
            diamond_blocks, 4u,
            diamond_edges, 4u,
            diamond_instructions, 4u,
            diamond_values, 1u,
            diamond_uses, 1u,
            NULL, 0u,
            NULL, 0u
        };
        archon_verify_report_reset(&report);
        assert(archon_verify_program(&diamond, &config, &report) ==
               ARCHON_VERIFY_INVALID_MODEL);
        assert(has_code(&report, ARCHON_VERIFY_DIAG_NOT_DOMINATED));
    }

    /* Stable IDs. */
    archon_verify_report_reset(&report);
    instructions[1].id = 100u;
    status = archon_verify_program(&program, &config, &report);
    assert(status == ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_DUPLICATE_ID));
    instructions[1].id = 101u;

    /* References and use-before-definition/dominance. */
    archon_verify_report_reset(&report);
    uses[0].value = 999u;
    assert(archon_verify_program(&program, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_INVALID_REFERENCE));
    uses[0].value = 200u;
    archon_verify_report_reset(&report);
    uses[0].instruction = 100u;
    assert(archon_verify_program(&program, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_USE_BEFORE_DEFINITION));
    uses[0].instruction = 101u;

    /* Block and effect ordering. */
    archon_verify_report_reset(&report);
    instructions[2].flags = 0u;
    assert(archon_verify_program(&program, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_MISSING_TERMINATOR));
    instructions[2].flags = ARCHON_VERIFY_INST_TERMINATOR;
    archon_verify_report_reset(&report);
    effect_dependencies[0].effect = 301u;
    assert(archon_verify_program(&program, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_BAD_EFFECT_ORDER));
    effect_dependencies[0].effect = 300u;

    /* Complete instruction maps. */
    archon_verify_report_reset(&report);
    instr_map.mapping_count = 2u;
    assert(archon_verify_instr_map(&instr_map, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_MISSING_MAPPING));
    instr_map.mapping_count = 3u;

    /* Dependency latency and resource capacity. */
    archon_verify_report_reset(&report);
    scheduled.issue_width = 2u;
    entries[1].cycle = 0u;
    entries[1].slot = 1u;
    assert(archon_verify_scheduled(&scheduled, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_BAD_SCHEDULE_ORDER));
    assert(has_code(&report, ARCHON_VERIFY_DIAG_RESOURCE_OVERBOOKED));
    scheduled.issue_width = 1u;
    entries[1].cycle = 1u;
    entries[1].slot = 0u;

    /* Register-map live ranges and final allocation policy. */
    archon_verify_report_reset(&report);
    live_ranges[0].end = 1u;
    assert(archon_verify_reg_map(&reg_map, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_BAD_LIVE_RANGE));
    live_ranges[0].end = 2u;
    archon_verify_report_reset(&report);
    allocations[0].physical_register = 801u;
    assert(archon_verify_regallocated(&regallocated, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_RESERVED_REGISTER));
    archon_verify_report_reset(&report);
    allocations[0].physical_register = 802u;
    assert(archon_verify_regallocated(&regallocated, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_ZERO_REGISTER));
    allocations[0].physical_register = 800u;
    archon_verify_report_reset(&report);
    allocations[0].location = ARCHON_VERIFY_LOCATION_STACK;
    allocations[0].physical_register = ARCHON_VERIFY_ID_NONE;
    allocations[0].spill_slot = 0;
    assert(archon_verify_regallocated(&regallocated, &config, &report) ==
           ARCHON_VERIFY_OK);
    allocations[0].location = ARCHON_VERIFY_LOCATION_REGISTER;
    allocations[0].physical_register = 800u;
    allocations[0].spill_slot = INT64_C(-1);
    {
        struct archon_verify_virtual_register overlapping_vregs[] = {
            {700u, 5u, ARCHON_VERIFY_ID_NONE, 0u},
            {701u, 5u, ARCHON_VERIFY_ID_NONE, 0u}
        };
        struct archon_verify_live_range overlapping_ranges[] = {
            {720u, 700u, 10u, 0u, 2u},
            {721u, 701u, 10u, 0u, 2u}
        };
        struct archon_verify_allocation overlapping_allocations[] = {
            {810u, 700u, ARCHON_VERIFY_LOCATION_REGISTER,
             800u, INT64_C(-1)},
            {811u, 701u, ARCHON_VERIFY_LOCATION_REGISTER,
             800u, INT64_C(-1)}
        };
        struct archon_verify_reg_map overlapping_map = {
            &program,
            overlapping_vregs, 2u,
            value_registers, 1u,
            overlapping_ranges, 2u
        };
        struct archon_verify_regallocated overlapping_allocated = {
            &overlapping_map, physical_registers, 3u,
            overlapping_allocations, 2u
        };
        archon_verify_report_reset(&report);
        assert(archon_verify_regallocated(
                   &overlapping_allocated, &config, &report) ==
               ARCHON_VERIFY_INVALID_MODEL);
        assert(has_code(&report, ARCHON_VERIFY_DIAG_INTERFERENCE));
    }
    archon_verify_report_reset(&report);
    regallocated.allocation_count = 0u;
    assert(archon_verify_regallocated(&regallocated, &config, &report) ==
           ARCHON_VERIFY_INVALID_MODEL);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_MISSING_ALLOCATION));
    regallocated.allocation_count = 1u;

    /* Limits are checked before table access. */
    archon_verify_config_init(&config);
    config.limits.max_instructions = 2u;
    archon_verify_report_reset(&report);
    assert(archon_verify_program(&program, &config, &report) ==
           ARCHON_VERIFY_LIMIT_EXCEEDED);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_LIMIT));

    /* Allocator failure is a result, never an abort. */
    archon_verify_config_init(&config);
    config.allocator.allocate = fail_allocate;
    config.allocator.deallocate = fail_deallocate;
    archon_verify_report_reset(&report);
    assert(archon_verify_program(&program, &config, &report) ==
           ARCHON_VERIFY_OUT_OF_MEMORY);
    assert(has_code(&report, ARCHON_VERIFY_DIAG_ALLOCATION));

    /* Diagnostic storage is bounded while total/dropped remain observable. */
    {
        struct archon_verify_diagnostic one[1];
        struct archon_verify_report bounded;
        archon_verify_config_init(&config);
        archon_verify_report_init(&bounded, one, 1u);
        instructions[2].flags = 0u;
        uses[0].value = 999u;
        effect_dependencies[0].effect = 301u;
        assert(archon_verify_program(&program, &config, &bounded) ==
               ARCHON_VERIFY_INVALID_MODEL);
        assert(bounded.count == 1u);
        assert(bounded.total_count >= 2u);
        assert(bounded.dropped_count == bounded.total_count - 1u);
        instructions[2].flags = ARCHON_VERIFY_INST_TERMINATOR;
        uses[0].value = 200u;
        effect_dependencies[0].effect = 300u;
    }

    /* A mismatched allocator pair is rejected without dereferencing data. */
    archon_verify_config_init(&config);
    config.allocator.allocate = fail_allocate;
    config.allocator.deallocate = NULL;
    assert(archon_verify_program(&program, &config, NULL) ==
           ARCHON_VERIFY_INVALID_ARGUMENT);

    return 0;
}
"""


class IRGenVerifierTests(unittest.TestCase):
    def compiler(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or shutil.which(command[0]) is None:
            self.skipTest("C99 compiler unavailable")
        return command

    def test_emit_contract_is_deterministic_and_context_neutral(self):
        context = {"sentinel": [1, 2, 3]}
        first = verifier.emit(context)
        second = verifier.emit(None)
        self.assertEqual(context, {"sentinel": [1, 2, 3]})
        self.assertEqual(first, second)
        self.assertEqual(set(first), {"Verify.c", "Verify.h"})
        self.assertIn("Program.i", first["Verify.h"])
        self.assertIn("Regallocated.i", first["Verify.h"])
        self.assertIn("archon_verify_pipeline", first["Verify.h"])

    def test_strict_c99_positive_malformed_fault_and_limit_fixtures(self):
        command = self.compiler()
        outputs = verifier.emit({})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in outputs.items():
                (root / name).write_text(text, encoding="ascii")
            (root / "test.c").write_text(_FIXTURE, encoding="ascii")
            subprocess.run(
                [
                    *command,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "Verify.c",
                    "test.c",
                    "-o",
                    "test",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "test")], cwd=root, check=True)


if __name__ == "__main__":
    unittest.main()
