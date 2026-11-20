"""Compile and exercise the generated graph runtime and Memory SSA IR."""

import copy
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen.graphs import emit as emit_graphs
from archon.irgen.memory_ssa import ALIASES, NAME, emit_ir


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"
PDP11 = ROOT / "stdspec" / "pdp11" / "root.arch"
JVM = ROOT / "stdspec" / "jvm" / "root.arch"
AMD64 = ROOT / "stdspec" / "amd64" / "root.arch"


class GraphsAndMemorySSA(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        rv32 = copy.deepcopy(compile_file(RV32))
        # Preserve a real encoding and operand schema while exercising the
        # explicit ArchDB boundary for instructions with no ArchSem body.
        next(row for row in rv32["instructions"] if row["name"] == "SUB")[
            "semantics"
        ] = None
        cls.context = {
            "architectures": [
                rv32,
                compile_file(PDP11),
                compile_file(JVM),
                compile_file(AMD64),
            ]
        }

    def compilers(self):
        commands = []
        configured = shlex.split(os.environ.get("CC", ""))
        candidates = [configured] if configured else []
        candidates.extend(([name] for name in ("gcc", "clang")))
        seen = set()
        for command in candidates:
            if not command or not shutil.which(command[0]):
                continue
            resolved = str(Path(shutil.which(command[0])).resolve())
            key = (resolved, *command[1:])
            if key not in seen:
                seen.add(key)
                commands.append(command)
        if not commands:
            self.skipTest("GCC or Clang is unavailable")
        return commands

    @staticmethod
    def write(root, files):
        for name, text in files.items():
            (root / name).write_text(text, encoding="utf-8", newline="\n")

    def compile_and_run(self, compiler, root, sources, *, definitions=()):
        executable = root / ("test-" + Path(compiler[0]).name)
        subprocess.run(
            [
                *compiler,
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                *definitions,
                *sources,
                "-o",
                executable,
            ],
            cwd=root,
            check=True,
        )
        return subprocess.run(
            [str(executable)], cwd=root, check=True, capture_output=True
        )

    def test_generator_contract_and_archdb_metadata(self):
        self.assertEqual(NAME, "memory-ssa")
        self.assertIn("mssa", ALIASES)
        first = emit_ir(self.context)
        second = emit_ir(self.context)
        self.assertEqual(first, second)
        self.assertEqual(set(first), {"IR.c", "IR.h"})
        self.assertEqual(set(emit_graphs(self.context)), {"Graphs.c", "Graphs.h"})
        self.assertIn("ARCHON_IR_OP_RISCV32_ADD", first["IR.h"])
        self.assertIn('"RISCV32"', first["IR.c"])
        self.assertIn('"ADD"', first["IR.c"])
        # An architecture-free generation remains valid and has no target guess.
        empty = emit_ir({"architectures": []})
        self.assertNotIn("RISCV32", empty["IR.c"])

    def test_owned_cfg_dfg_pdg_strict_c99(self):
        files = emit_graphs(self.context)
        for compiler in self.compilers():
            with self.subTest(compiler=compiler[0]), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.write(root, files)
                (root / "graph_harness.c").write_text(
                    _GRAPH_HARNESS, encoding="ascii", newline="\n"
                )
                result = self.compile_and_run(
                    compiler, root, ["Graphs.c", "graph_harness.c"]
                )
                self.assertIn(b'"kind":"cfg"', result.stdout)
                self.assertIn(b'"dependency":"control"', result.stdout)

    def test_memory_ssa_build_verify_print_and_program_i(self):
        files = {}
        files.update(emit_graphs(self.context))
        files.update(emit_ir(self.context))
        for compiler in self.compilers():
            with self.subTest(compiler=compiler[0]), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                self.write(root, files)
                (root / "memory_harness.c").write_text(
                    _MEMORY_HARNESS, encoding="ascii", newline="\n"
                )
                (root / "Program.i").write_text(
                    _PROGRAM_I, encoding="ascii", newline="\n"
                )
                result = self.compile_and_run(
                    compiler,
                    root,
                    ["Graphs.c", "IR.c", "memory_harness.c"],
                    definitions=('-DARCHON_IR_PROGRAM_FILE="Program.i"',),
                )
                again = subprocess.run(
                    [str(root / ("test-" + Path(compiler[0]).name))],
                    cwd=root,
                    check=True,
                    capture_output=True,
                )
                self.assertEqual(result.stdout, again.stdout)
                text = result.stdout.decode("ascii")
                self.assertIn("memory-ssa {", text)
                self.assertIn("memory.phi", text)
                self.assertIn("target RISCV32.ADD", text)
                self.assertIn("target RISCV32.SUB", text)
                self.assertIn("target PDP11.ADC", text)
                self.assertIn("target JVM.ICONST_0", text)
                self.assertIn('call "side"', text)


_GRAPH_HARNESS = r"""#include "Graphs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    ArchonCFG cfg;
    ArchonDFG dfg;
    ArchonPDG pdg;
    ArchonGraphNodeId entry, left, right, exit_node, first, second, third;
    ArchonGraphNodeId adjacent, order[4];
    ArchonGraphEdgeId edge, adjacent_edge;
    uint32_t components[4];
    size_t count, component_count, length, second_length;
    char message[128];
    char *serialized, *serialized_again;
    int answer;
    FILE *stream;

    assert(archon_cfg_init(&cfg) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_block(&cfg, "entry", &entry) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_block(&cfg, "left", &left) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_block(&cfg, "right", &right) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_block(&cfg, "exit\nblock", &exit_node) == ARCHON_GRAPH_OK);
    assert(entry == 0u && left == 1u && right == 2u && exit_node == 3u);
    assert(archon_graph_set_entry(&cfg, entry) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_edge(&cfg, entry, right, "false", &edge) == ARCHON_GRAPH_OK);
    assert(edge == 0u);
    assert(archon_cfg_add_edge(&cfg, entry, left, "true", &edge) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_edge(&cfg, left, exit_node, "join", &edge) == ARCHON_GRAPH_OK);
    assert(archon_cfg_add_edge(&cfg, right, exit_node, "join", &edge) == ARCHON_GRAPH_OK);
    assert(archon_graph_validate(&cfg, message, sizeof message) == ARCHON_GRAPH_OK);
    assert(archon_graph_successor_count(&cfg, entry) == 2u);
    assert(archon_graph_predecessor_count(&cfg, exit_node) == 2u);
    assert(archon_graph_successor(
        &cfg, entry, 0u, &adjacent, &adjacent_edge) == ARCHON_GRAPH_OK);
    assert(adjacent == right && adjacent_edge == 0u);
    assert(archon_graph_predecessor(
        &cfg, exit_node, 0u, &adjacent, &adjacent_edge) == ARCHON_GRAPH_OK);
    assert(adjacent == left);
    assert(archon_graph_reachable(&cfg, entry, exit_node, &answer) == ARCHON_GRAPH_OK);
    assert(answer);
    assert(archon_cfg_dominates(&cfg, entry, exit_node, &answer) == ARCHON_GRAPH_OK);
    assert(answer);
    assert(archon_cfg_dominates(&cfg, left, exit_node, &answer) == ARCHON_GRAPH_OK);
    assert(!answer);
    assert(archon_graph_topological_sort(&cfg, order, 4u, &count) == ARCHON_GRAPH_OK);
    assert(count == 4u && order[0] == entry && order[1] == left &&
           order[2] == right && order[3] == exit_node);
    assert(archon_graph_serialize(&cfg, &serialized, &length) == ARCHON_GRAPH_OK);
    assert(archon_graph_serialize(
        &cfg, &serialized_again, &second_length) == ARCHON_GRAPH_OK);
    assert(length == second_length && strcmp(serialized, serialized_again) == 0);
    assert(strstr(serialized, "exit\\nblock") != NULL);
    assert(fwrite(serialized, 1u, length, stdout) == length);
    stream = tmpfile();
    assert(stream != NULL);
    assert(archon_graph_write(&cfg, stream) == ARCHON_GRAPH_OK);
    assert(fclose(stream) == 0);
    archon_graph_free_serialized(serialized);
    archon_graph_free_serialized(serialized_again);

    assert(archon_dfg_init(&dfg) == ARCHON_GRAPH_OK);
    assert(archon_dfg_add_value(&dfg, "definition", &first) == ARCHON_GRAPH_OK);
    assert(archon_dfg_add_value(&dfg, "middle", &second) == ARCHON_GRAPH_OK);
    assert(archon_dfg_add_value(&dfg, "user", &third) == ARCHON_GRAPH_OK);
    assert(archon_dfg_add_use(&dfg, first, second, 0u, "lhs", &edge) == ARCHON_GRAPH_OK);
    assert(archon_dfg_add_use(&dfg, first, third, 1u, "rhs", &edge) == ARCHON_GRAPH_OK);
    assert(archon_dfg_add_use(&dfg, second, third, 0u, "value", &edge) == ARCHON_GRAPH_OK);
    assert(archon_graph_topological_sort(&dfg, order, 4u, &count) == ARCHON_GRAPH_OK);
    assert(count == 3u && order[0] == first && order[1] == second && order[2] == third);
    assert(archon_graph_validate(&dfg, message, sizeof message) == ARCHON_GRAPH_OK);

    assert(archon_pdg_init(&pdg) == ARCHON_GRAPH_OK);
    assert(archon_pdg_add_node(&pdg, "load", &first) == ARCHON_GRAPH_OK);
    assert(archon_pdg_add_node(&pdg, "store", &second) == ARCHON_GRAPH_OK);
    assert(archon_pdg_add_node(&pdg, "return", &third) == ARCHON_GRAPH_OK);
    assert(archon_pdg_add_dependency(
        &pdg, first, second, ARCHON_DEPENDENCY_MEMORY, 0u, "RAW", &edge) ==
        ARCHON_GRAPH_OK);
    assert(archon_pdg_add_dependency(
        &pdg, second, first, ARCHON_DEPENDENCY_ANTI, 0u, "WAR", &edge) ==
        ARCHON_GRAPH_OK);
    assert(archon_pdg_add_dependency(
        &pdg, second, third, ARCHON_DEPENDENCY_ORDER, 0u, "order", &edge) ==
        ARCHON_GRAPH_OK);
    assert(archon_graph_topological_sort(&pdg, order, 4u, &count) ==
        ARCHON_GRAPH_CYCLE);
    assert(archon_graph_strong_components(
        &pdg, components, 4u, &component_count) == ARCHON_GRAPH_OK);
    assert(component_count == 2u);
    assert(components[first] == 0u && components[second] == 0u &&
           components[third] == 1u);
    assert(archon_graph_validate(&pdg, message, sizeof message) == ARCHON_GRAPH_OK);
    pdg.edges[0].target = ARCHON_GRAPH_INVALID_NODE;
    assert(archon_graph_validate(&pdg, message, sizeof message) ==
        ARCHON_GRAPH_INVALID_GRAPH);
    assert(strstr(message, "edge") != NULL);
    pdg.edges[0].target = second;
    assert(strcmp(archon_dependency_kind_name(ARCHON_DEPENDENCY_MEMORY), "memory") == 0);

    archon_graph_dispose(&cfg);
    archon_graph_dispose(&dfg);
    archon_graph_dispose(&pdg);
    return 0;
}
"""


_MEMORY_HARNESS = r"""#include "IR.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct IR_program program, decoded, transactional;
    struct IR_builder builder;
    struct IR_diagnostic diagnostic;
    struct IR_phi_input value_inputs[2], memory_inputs[2];
    const struct IR_operation_metadata *metadata;
    const struct IR_operand_metadata *operand;
    IR_block_id entry, left, right, merge;
    IR_value_id memory0, pointer, condition, constant, constant16;
    IR_value_id constant64a, constant64b;
    IR_value_id left_value, left_memory, right_value, right_memory;
    IR_value_id phi_value, phi_memory, target_value, final_memory;
    IR_value_id opaque_value, opaque_memory, state_value, state_memory;
    IR_value_id stack_memory;
    IR_value_id arguments[1], target_operands[2], one_operand[1];
    IR_value_id wide_operands[2], saved_memory;
    enum IR_operation operation, opaque_operation, state_operation;
    enum IR_operation stack_operation, multi_operation;
    size_t node_index;

    assert(IR_program_init(&program) == IR_OK);
    assert(IR_add_block(&program, "entry", &entry) == IR_OK);
    assert(IR_add_block(&program, "left", &left) == IR_OK);
    assert(IR_add_block(&program, "right", &right) == IR_OK);
    assert(IR_add_block(&program, "merge", &merge) == IR_OK);
    assert(IR_set_entry(&program, entry) == IR_OK);
    assert(IR_builder_init(&builder, &program, entry) == IR_OK);
    assert(IR_build_memory_parameter(&builder, "memory", &memory0) == IR_OK);
    assert(IR_build_parameter(
        &builder, IR_type_make(IR_TYPE_POINTER, 32u, 0u),
        "address", &pointer) == IR_OK);
    assert(IR_build_parameter(
        &builder, IR_type_make(IR_TYPE_UINT, 1u, 0u),
        "condition", &condition) == IR_OK);
    assert(IR_build_constant(
        &builder, IR_type_make(IR_TYPE_BITS, 32u, 0u),
        UINT64_C(7), "seven", &constant) == IR_OK);
    assert(IR_build_constant(
        &builder, IR_type_make(IR_TYPE_BITS, 16u, 0u),
        UINT64_C(3), "three16", &constant16) == IR_OK);
    assert(IR_build_constant(
        &builder, IR_type_make(IR_TYPE_BITS, 64u, 0u),
        UINT64_C(11), "eleven64", &constant64a) == IR_OK);
    assert(IR_build_constant(
        &builder, IR_type_make(IR_TYPE_BITS, 64u, 0u),
        UINT64_C(12), "twelve64", &constant64b) == IR_OK);
    assert(IR_build_branch(&builder, condition, left, right) == IR_OK);

    assert(IR_builder_position(&builder, left) == IR_OK);
    assert(IR_build_load(
        &builder, IR_type_make(IR_TYPE_BITS, 32u, 0u),
        pointer, memory0, 4u, "loaded", &left_value) == IR_OK);
    assert(IR_build_store(
        &builder, pointer, left_value, memory0, 4u, &left_memory) == IR_OK);
    assert(IR_build_jump(&builder, merge) == IR_OK);

    assert(IR_builder_position(&builder, right) == IR_OK);
    arguments[0] = constant;
    assert(IR_build_call(
        &builder, "side", arguments, 1u,
        IR_type_make(IR_TYPE_BITS, 32u, 0u), memory0,
        IR_EFFECT_READ_MEMORY | IR_EFFECT_WRITE_MEMORY,
        &right_value, &right_memory) == IR_OK);
    assert(IR_build_jump(&builder, merge) == IR_OK);

    assert(IR_builder_position(&builder, merge) == IR_OK);
    value_inputs[0].block = left;
    value_inputs[0].value = left_value;
    value_inputs[1].block = right;
    value_inputs[1].value = right_value;
    assert(IR_build_phi(
        &builder, IR_type_make(IR_TYPE_BITS, 32u, 0u),
        value_inputs, 2u, "selected", &phi_value) == IR_OK);
    memory_inputs[0].block = left;
    memory_inputs[0].value = left_memory;
    memory_inputs[1].block = right;
    memory_inputs[1].value = right_memory;
    assert(IR_build_memory_phi(
        &builder, memory_inputs, 2u, "joined-memory", &phi_memory) == IR_OK);

    assert(IR_architecture_count() == 4u);
    assert(strcmp(IR_architecture(0u)->name, "RISCV32") == 0);
    operation = IR_find_operation("RISCV32", "ADD");
    assert(operation == ARCHON_IR_OP_RISCV32_ADD);
    metadata = IR_operation_metadata(operation);
    assert(metadata != NULL && metadata->operand_count == 3u);
    assert(metadata->input_count == 2u && metadata->result_count == 1u);
    assert(metadata->semantics_known && metadata->effects_known);
    assert(!metadata->equivalence_known && !metadata->requires_sequence);
    assert(metadata->effects == IR_EFFECT_NONE);
    operand = IR_operation_operand(operation, 0u);
    assert(operand != NULL && operand->kind == IR_OPERAND_REGISTER &&
           operand->direction == IR_OPERAND_OUT);
    assert(operand->value_width == 32u);
    assert(IR_operation_input(operation, 0u)->direction == IR_OPERAND_IN);
    assert(strcmp(IR_operation_input(operation, 0u)->name, "rs1") == 0);
    assert(strcmp(IR_operation_result(operation, 0u)->name, "rd") == 0);
    target_operands[0] = phi_value;
    target_operands[1] = constant;
    assert(IR_build_target(
        &builder, operation, target_operands, 2u,
        IR_type_make(IR_TYPE_BITS, 32u, 0u), IR_NO_VALUE,
        &target_value, NULL) == IR_OK);

    opaque_operation = IR_find_operation("RISCV32", "SUB");
    metadata = IR_operation_metadata(opaque_operation);
    assert(metadata != NULL && !metadata->semantics_known);
    assert(!metadata->effects_known && !metadata->equivalence_known);
    assert(metadata->requires_sequence);
    assert((metadata->effects & IR_EFFECT_OPAQUE) != 0u);
    assert((metadata->effects & IR_EFFECT_SIDE_EFFECT) != 0u);
    target_operands[0] = target_value;
    target_operands[1] = constant;
    assert(IR_build_target(
        &builder, opaque_operation, target_operands, 2u,
        IR_type_make(IR_TYPE_BITS, 32u, 0u), IR_NO_VALUE,
        &opaque_value, &opaque_memory) == IR_INVALID_ARGUMENT);
    assert(IR_build_target(
        &builder, opaque_operation, target_operands, 2u,
        IR_type_make(IR_TYPE_BITS, 32u, 0u), phi_memory,
        &opaque_value, &opaque_memory) == IR_OK);

    state_operation = IR_find_operation("PDP11", "ADC");
    metadata = IR_operation_metadata(state_operation);
    assert(metadata != NULL && metadata->semantics_known &&
           metadata->effects_known && metadata->requires_sequence);
    assert((metadata->effects & IR_EFFECT_READ_SCALAR) != 0u);
    assert((metadata->effects & IR_EFFECT_WRITE_SCALAR) != 0u);
    assert((metadata->effects & IR_EFFECT_SIDE_EFFECT) != 0u);
    one_operand[0] = constant16;
    assert(IR_build_target(
        &builder, state_operation, one_operand, 1u,
        IR_type_make(IR_TYPE_BITS, 16u, 0u), IR_NO_VALUE,
        &state_value, &state_memory) == IR_INVALID_ARGUMENT);
    assert(IR_build_target(
        &builder, state_operation, one_operand, 1u,
        IR_type_make(IR_TYPE_BITS, 16u, 0u), opaque_memory,
        &state_value, &state_memory) == IR_OK);

    stack_operation = IR_find_operation("JVM", "ICONST_0");
    metadata = IR_operation_metadata(stack_operation);
    assert(metadata != NULL && metadata->semantics_known &&
           metadata->effects_known && metadata->requires_sequence);
    assert(metadata->input_count == 0u && metadata->result_count == 0u);
    assert((metadata->effects & IR_EFFECT_WRITE_STACK) != 0u);
    assert(IR_build_target(
        &builder, stack_operation, NULL, 0u,
        IR_type_make(IR_TYPE_VOID, 0u, 0u), IR_NO_VALUE,
        NULL, &stack_memory) == IR_INVALID_ARGUMENT);
    assert(IR_build_target(
        &builder, stack_operation, NULL, 0u,
        IR_type_make(IR_TYPE_VOID, 0u, 0u), state_memory,
        NULL, &stack_memory) == IR_OK);

    multi_operation = IR_find_operation("AMD64", "XCHG");
    metadata = IR_operation_metadata(multi_operation);
    assert(metadata != NULL && metadata->input_count == 2u &&
           metadata->result_count == 2u);
    wide_operands[0] = constant64a;
    wide_operands[1] = constant64b;
    assert(IR_build_target(
        &builder, multi_operation, wide_operands, 2u,
        IR_type_make(IR_TYPE_BITS, 64u, 0u), IR_NO_VALUE,
        &target_value, NULL) == IR_UNSUPPORTED);

    assert(IR_build_store(
        &builder, pointer, opaque_value, stack_memory, 4u,
        &final_memory) == IR_OK);
    assert(IR_build_return(&builder, opaque_value, final_memory) == IR_OK);

    assert(IR_get_block(&program, merge) != NULL);
    assert(IR_get_node(&program, 0u) != NULL);
    assert(IR_get_value(&program, final_memory)->type.kind == IR_TYPE_MEMORY);
    assert(IR_get_value(&program, final_memory)->memory_version == 7u);
    assert(IR_verify(&program, &diagnostic) == IR_OK);
    assert(IR_print(stdout, &program, &diagnostic) == IR_OK);

    for (node_index = 0u; node_index < program.node_count; ++node_index)
        if (program.nodes[node_index].kind == IR_NODE_LOAD)
            break;
    assert(node_index < program.node_count);
    saved_memory = program.nodes[node_index].memory_input;
    program.nodes[node_index].memory_input = constant;
    assert(IR_verify(&program, &diagnostic) == IR_TYPE_MISMATCH);
    assert(strstr(diagnostic.message, "memory") != NULL);
    program.nodes[node_index].memory_input = saved_memory;
    assert(IR_verify(&program, &diagnostic) == IR_OK);
    IR_program_destroy(&program);

    assert(IR_load_program(&decoded, &diagnostic) == IR_OK);
    assert(decoded.block_count == 1u);
    assert(decoded.node_count == 2u);
    assert(decoded.value_count == 1u);
    assert(IR_verify(&decoded, &diagnostic) == IR_OK);
    IR_program_destroy(&decoded);

    assert(IR_program_init(&transactional) == IR_OK);
    assert(IR_add_block(&transactional, "from", &entry) == IR_OK);
    assert(IR_add_block(&transactional, "to", &left) == IR_OK);
    assert(IR_set_entry(&transactional, entry) == IR_OK);
    assert(IR_builder_init(&builder, &transactional, entry) == IR_OK);
    transactional.cfg.kind = ARCHON_GRAPH_DFG;
    assert(IR_build_jump(&builder, left) == IR_MALFORMED);
    assert(transactional.node_count == 0u);
    assert(transactional.blocks[entry].node_count == 0u);
    assert(transactional.cfg.edge_count == 0u);
    transactional.cfg.kind = ARCHON_GRAPH_CFG;
    IR_program_destroy(&transactional);
    return 0;
}
"""


_PROGRAM_I = r"""ARCHON_IR_DECLARE_BLOCK(0, "macro-entry");
ARCHON_IR_ENTRY(0);
ARCHON_IR_BLOCK(0);
ARCHON_IR_CONSTANT(0, IR_TYPE_UINT, 32, 7, "seven");
ARCHON_IR_RETURN(0, IR_NO_VALUE);
"""


if __name__ == "__main__":
    unittest.main()
