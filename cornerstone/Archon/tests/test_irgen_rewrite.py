"""Compile and execute the generated Sea-of-Nodes and rewrite runtimes."""

import copy
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import graphs, oracle, rewrite, sea_of_nodes


ROOT = Path(__file__).resolve().parents[1]
STDSPEC = ROOT / "stdspec"


class GeneratedIRRewriteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rv32 = compile_file(STDSPEC / "riscv" / "rv32i.arch")

    def compiler(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("C99 compiler unavailable")
        return compiler

    def compile_and_run(self, files, test, *, defines=()):
        compiler = self.compiler()
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            for name, source in files.items():
                (root / name).write_text(source, encoding="ascii")
            (root / "test.c").write_text(test, encoding="ascii")
            sources = [
                name
                for name in ("Graphs.c", "IR.c", "Oracle.c", "Rewrite.c")
                if name in files
            ]
            subprocess.run(
                [
                    *compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    *[f"-D{name}" for name in defines],
                    *sources,
                    "test.c",
                    "-o",
                    "test",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "test")], cwd=root, check=True)

    def test_sea_of_nodes_builders_verifier_and_program_decoder(self):
        files = sea_of_nodes.emit_ir(None)
        self.assertEqual(set(files), {"IR.c", "IR.h"})
        self.assertEqual(sea_of_nodes.NAME, "sea-of-nodes")
        targeted = sea_of_nodes.emit_ir({"architectures": [self.rv32]})
        self.assertIn("ARCHON_IR_OP_RISCV32_ADD", targeted["IR.h"])
        self.assertIn("RISCV32", targeted["IR.c"])
        files["Program.i"] = r"""
ARCHON_IR_PROGRAM_BEGIN(1u, 2u, 1u)
ARCHON_IR_NODE(0u, ARCHON_IR_NODE_CONSTANT, 7u, 0u,
               ARCHON_IR_NODE_NONE, UINT64_C(9), 0u)
ARCHON_IR_NODE(1u, ARCHON_IR_NODE_GENERIC, 8u, ARCHON_IR_FLAG_PURE,
               ARCHON_IR_NODE_NONE, UINT64_C(0), 1u)
ARCHON_IR_INPUT(1u, 0u, ARCHON_IR_INPUT_DATA, 0u)
ARCHON_IR_ROOT(1u)
ARCHON_IR_PROGRAM_END()
"""
        test = r"""
#include "IR.h"
#include <assert.h>

int main(void) {
    ArchonIRGraph graph;
    ArchonIRNodeId start, left, right, merge, one, two, phi, root;
    ArchonIRInput control;
    ArchonIRNodeId predecessors[2], values[2], data[1];
    ArchonIRBuilder builder;
    ArchonIRProgramDecoder decoder;

    assert(archon_ir_graph_init(&graph, NULL) == ARCHON_IR_OK);
    assert(archon_ir_add_node(&graph, ARCHON_IR_NODE_START, 0u, 0u,
        ARCHON_IR_NODE_NONE, 0u, NULL, 0u, &start) == ARCHON_IR_OK);
    control.source = start;
    control.kind = ARCHON_IR_INPUT_CONTROL;
    control.reserved[0] = control.reserved[1] = control.reserved[2] = 0u;
    assert(archon_ir_add_node(&graph, ARCHON_IR_NODE_GENERIC, 1u, 0u,
        ARCHON_IR_NODE_NONE, 0u, &control, 1u, &left) == ARCHON_IR_OK);
    assert(archon_ir_add_node(&graph, ARCHON_IR_NODE_GENERIC, 2u, 0u,
        ARCHON_IR_NODE_NONE, 0u, &control, 1u, &right) == ARCHON_IR_OK);
    predecessors[0] = left;
    predecessors[1] = right;
    assert(archon_ir_add_merge(&graph, predecessors, 2u, &merge)
        == ARCHON_IR_OK);
    assert(archon_ir_add_node(&graph, ARCHON_IR_NODE_CONSTANT, 3u,
        ARCHON_IR_FLAG_PURE, ARCHON_IR_NODE_NONE, 1u, NULL, 0u, &one)
        == ARCHON_IR_OK);
    assert(archon_ir_add_node(&graph, ARCHON_IR_NODE_CONSTANT, 3u,
        ARCHON_IR_FLAG_PURE, ARCHON_IR_NODE_NONE, 2u, NULL, 0u, &two)
        == ARCHON_IR_OK);
    values[0] = one;
    values[1] = two;
    assert(archon_ir_add_phi(&graph, merge, values, 2u, &phi)
        == ARCHON_IR_OK);
    archon_ir_builder_init(&builder, &graph);
    assert(archon_ir_builder_position(
        &builder, merge, merge, ARCHON_IR_NODE_NONE) == ARCHON_IR_OK);
    data[0] = phi;
    assert(archon_ir_builder_emit(&builder, ARCHON_IR_NODE_GENERIC, 4u,
        ARCHON_IR_FLAG_PURE, 0u, data, 1u, &root) == ARCHON_IR_OK);
    assert(root == 7u);
    assert(archon_ir_node(&graph, phi)->id == phi);
    assert(archon_ir_verify(&graph, root) == ARCHON_IR_OK);
    --graph.nodes[phi].first_input;
    assert(archon_ir_verify(&graph, root) == ARCHON_IR_MALFORMED_GRAPH);
    ++graph.nodes[phi].first_input;
    graph.inputs[graph.nodes[phi].first_input].kind =
        ARCHON_IR_INPUT_EFFECT;
    assert(archon_ir_verify(&graph, root) == ARCHON_IR_MALFORMED_GRAPH);
    archon_ir_graph_destroy(&graph);

    assert(archon_ir_graph_init(&graph, NULL) == ARCHON_IR_OK);
    assert(archon_ir_program_begin(
        &decoder, &graph, 1u, 2u, 1u) == ARCHON_IR_OK);
    assert(archon_ir_program_node(
        &decoder, 0u, ARCHON_IR_NODE_GENERIC, 1u, 0u,
        ARCHON_IR_NODE_NONE, 0u, 1u) == ARCHON_IR_OK);
    assert(archon_ir_program_node(
        &decoder, 1u, ARCHON_IR_NODE_GENERIC, 2u, 0u,
        ARCHON_IR_NODE_NONE, 0u, 1u) == ARCHON_IR_MALFORMED_PROGRAM);
    archon_ir_program_abort(&decoder);
    assert(graph.node_count == 0u);
    archon_ir_graph_destroy(&graph);

    assert(archon_ir_graph_init(&graph, NULL) == ARCHON_IR_OK);
    assert(archon_ir_decode_program(&graph, &root) == ARCHON_IR_OK);
    assert(root == 1u && graph.node_count == 2u);
    assert(archon_ir_node(&graph, 0u)->payload == UINT64_C(9));
    assert(archon_ir_verify(&graph, root) == ARCHON_IR_OK);
    archon_ir_graph_destroy(&graph);
    return 0;
}
"""
        self.compile_and_run(
            files, test, defines=("ARCHON_IR_INCLUDE_PROGRAM",)
        )

    def test_pattern_bank_is_conservative_and_has_a_safe_macro_schema(self):
        database = copy.deepcopy(self.rv32)
        database["architecture"] = "RV-32/test"
        opaque = copy.deepcopy(database["instructions"][0])
        opaque["name"] = "OPAQUE"
        opaque["semantics"] = None
        database["instructions"].append(opaque)
        files = rewrite.emit({
            "architectures": [database],
            "options": {"oracle": False},
        })
        bank_name = "PatternBank_RV_32_test.i"
        self.assertIn(bank_name, files)
        bank = files[bank_name]
        self.assertIn('ARCHON_TARGET_FACT("OPAQUE"', bank)
        self.assertNotIn("lower.OPAQUE", bank)
        self.assertIn("lower.ADD", bank)
        self.assertIn("ARCHON_COMMUTATIVE_RULE", bank)
        collision = copy.deepcopy(database)
        collision["architecture"] = "RV_32_test"
        collision_files = rewrite.emit(
            {
                "architectures": [database, collision],
                "options": {"oracle": False},
            }
        )
        collision_banks = [
            name
            for name in collision_files
            if name.startswith("PatternBank_")
        ]
        self.assertEqual(len(collision_banks), 2)
        self.assertEqual(
            len({name.casefold() for name in collision_banks}), 2
        )
        files = {bank_name: bank}
        test = f"""
#include <stdint.h>
#define ARCHON_PATTERN_BANK_BEGIN(...)
#define ARCHON_TARGET_FACT(...)
#define ARCHON_RENAME_RULE(...)
#define ARCHON_IDENTITY_RULE(...)
#define ARCHON_COMMUTATIVE_RULE(...)
#define ARCHON_PATTERN_BANK_END(...)
#include "{bank_name}"
int main(void) {{ return 0; }}
"""
        self.compile_and_run(files, test)

    def test_typed_oracle_rewrite_advice_is_bounded_and_never_proof(self):
        files = {
            **graphs.emit(None),
            **oracle.emit(None),
            **rewrite.emit({"architectures": [self.rv32]}),
        }
        files["Rewritten.oracle.i"] = """\
ARCHON_ORACLE_BEGIN(1, 1)
ARCHON_ORACLE_BOOL("rewrite.enabled", 1)
ARCHON_ORACLE_STRING("rewrite.engine", "egraph")
ARCHON_ORACLE_U64("rewrite.max_iterations", UINT64_C(3))
ARCHON_ORACLE_BOOL("rewrite.operation.7.forbid", 1)
ARCHON_ORACLE_I64("rewrite.operation.7.priority", INT64_C(19))
ARCHON_ORACLE_STRING("unknown.proof", "proved")
ARCHON_ORACLE_END()
"""
        test = r"""
#include "Rewrite.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    ArchonRewriteOracleConfig config;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    int forbidden = 0;
    int64_t priority = 0;

    status = archon_rewrite_oracle_config_load(
        "Rewritten.oracle.i", ARCHON_REWRITE_DIRECTED, 16u,
        &config, &diagnostic);
    assert(status == ARCHON_ORACLE_STATUS_OK);
    assert(config.loaded && config.enabled);
    assert(config.engine == ARCHON_REWRITE_EQUALITY_SATURATION);
    assert(config.max_iterations == 3u);
    assert(config.annotations.count == 6u);
    assert(archon_rewrite_oracle_operation(
               &config, UINT64_C(7), &forbidden, &priority,
               &diagnostic) == ARCHON_ORACLE_STATUS_OK);
    assert(forbidden && priority == INT64_C(19));
    assert(strcmp(
        config.annotations.entries[5].key, "unknown.proof") == 0);
    archon_rewrite_oracle_config_dispose(&config);
    return 0;
}
"""
        self.compile_and_run(files, test)

    def test_trailing_program_records_do_not_commit_partial_graphs(self):
        files = {
            **graphs.emit(None),
            **sea_of_nodes.emit_ir(None),
            **rewrite.emit({"options": {"oracle": False}}),
        }
        files["Program.i"] = r"""
ARCHON_IR_PROGRAM_BEGIN(ARCHON_IR_PROGRAM_SCHEMA, 1u, 0u)
ARCHON_IR_NODE(0u, ARCHON_IR_NODE_CONSTANT, 7u, ARCHON_IR_FLAG_NONE,
               ARCHON_IR_NODE_NONE, UINT64_C(9), 0u)
ARCHON_IR_ROOT(0u)
ARCHON_IR_PROGRAM_END()
ARCHON_IR_PROGRAM_END()
"""
        test = r"""
#include "IR.h"
#include "Rewrite.h"
#include <assert.h>

int main(void) {
    ArchonIRGraph ir;
    ArchonIRNodeId sentinel, root = 123u;
    ArchonRewriteGraph terms;
    ArchonGraphsAPI api;
    ArchonGraphsNodeId rewritten = 123u;

    assert(archon_ir_graph_init(&ir, NULL) == ARCHON_IR_OK);
    assert(archon_ir_add_node(
        &ir, ARCHON_IR_NODE_CONSTANT, 9u, ARCHON_IR_FLAG_PURE,
        ARCHON_IR_NODE_NONE, UINT64_C(42), NULL, 0u, &sentinel)
        == ARCHON_IR_OK);
    assert(archon_ir_decode_program(&ir, &root)
        == ARCHON_IR_MALFORMED_PROGRAM);
    assert(root == 123u && ir.node_count == 1u);
    assert(archon_ir_node(&ir, sentinel)->payload == UINT64_C(42));
    archon_ir_graph_destroy(&ir);

    assert(archon_rewrite_graph_init(&terms) == ARCHON_REWRITE_OK);
    api = archon_rewrite_graph_api(&terms);
    assert(archon_rewrite_program(
        &api, ARCHON_REWRITE_DIRECTED, NULL, &rewritten, NULL)
        == ARCHON_REWRITE_MALFORMED_PROGRAM);
    assert(rewritten == 123u && terms.term_count == 0u);
    archon_rewrite_graph_dispose(&terms);
    return 0;
}
"""
        self.compile_and_run(
            files,
            test,
            defines=(
                "ARCHON_IR_INCLUDE_PROGRAM",
                "ARCHON_REWRITE_INCLUDE_PROGRAM",
            ),
        )

    def test_directed_and_equality_engines_and_program_input(self):
        files = {
            **graphs.emit(None),
            **sea_of_nodes.emit_ir({"architectures": [self.rv32]}),
            **rewrite.emit({
                "architectures": [self.rv32],
                "options": {"oracle": False},
            }),
        }
        bank = files["PatternBank_RISCV32.i"]
        add_opcode = re.search(
            r'ARCHON_TARGET_FACT\("ADD", UINT32_C\((0x[0-9a-f]+)\)', bank
        )[1]
        constant_opcode = re.search(
            r"ARCHON_IDENTITY_RULE\([^\n]+UINT32_C\((0x[0-9a-f]+)\), "
            r"UINT64_C\(0x0000000000000000\)",
            bank,
        )[1]
        files["Program.i"] = f"""
ARCHON_IR_PROGRAM_BEGIN(1u, 3u, 2u)
ARCHON_IR_NODE(0u, 0u, UINT32_C(7), 0u, UINT32_MAX,
               UINT64_C(3), 0u)
ARCHON_IR_NODE(1u, 7u, UINT32_C({constant_opcode}), 0u, UINT32_MAX,
               UINT64_C(0), 0u)
ARCHON_IR_NODE(2u, 0u, UINT32_C({add_opcode}), 0u, UINT32_MAX,
               UINT64_C(0), 2u)
ARCHON_IR_INPUT(2u, 0u, 1u, 0u)
ARCHON_IR_INPUT(2u, 1u, 1u, 1u)
ARCHON_IR_ROOT(2u)
ARCHON_IR_PROGRAM_END()
"""
        test = r"""
#include "IR.h"
#include "Rewrite.h"
#include <assert.h>
#include <string.h>

#define CAPACITY 256u
struct Node {
    uint32_t opcode, flags;
    uint64_t payload;
    ArchonGraphsNodeId children[ARCHON_REWRITE_MAX_ARITY];
    size_t child_count;
};
struct Graph { struct Node nodes[CAPACITY]; size_t count; };

static int read_node(
        void *context, ArchonGraphsNodeId id, ArchonGraphsNodeView *view) {
    struct Graph *graph = (struct Graph *)context;
    if ((size_t)id >= graph->count)
        return 1;
    view->opcode = graph->nodes[id].opcode;
    view->flags = graph->nodes[id].flags;
    view->payload = graph->nodes[id].payload;
    view->children = graph->nodes[id].children;
    view->child_count = graph->nodes[id].child_count;
    return 0;
}

static int intern_node(
        void *context, uint32_t opcode, uint64_t payload, uint32_t flags,
        const ArchonGraphsNodeId *children, size_t child_count,
        ArchonGraphsNodeId *result) {
    struct Graph *graph = (struct Graph *)context;
    size_t index;
    for (index = 0u; index < graph->count; ++index) {
        struct Node *node = &graph->nodes[index];
        if (node->opcode == opcode && node->payload == payload
                && node->flags == flags && node->child_count == child_count
                && memcmp(node->children, children,
                          child_count * sizeof(*children)) == 0) {
            *result = (ArchonGraphsNodeId)index;
            return 0;
        }
    }
    if (graph->count == CAPACITY
            || child_count > ARCHON_REWRITE_MAX_ARITY)
        return 1;
    graph->nodes[graph->count].opcode = opcode;
    graph->nodes[graph->count].payload = payload;
    graph->nodes[graph->count].flags = flags;
    graph->nodes[graph->count].child_count = child_count;
    if (child_count)
        memcpy(graph->nodes[graph->count].children, children,
               child_count * sizeof(*children));
    *result = (ArchonGraphsNodeId)graph->count++;
    return 0;
}

static size_t node_count(void *context) {
    return ((struct Graph *)context)->count;
}

static uint64_t tie_cost(
        void *context, uint32_t opcode, uint64_t payload, uint32_t flags) {
    (void)context;
    (void)payload;
    (void)flags;
    return opcode == 7u ? UINT64_C(1) : UINT64_C(0);
}

static uint32_t fact_opcode(const char *name) {
    size_t index;
    for (index = 0u; index < archon_generated_target_fact_count; ++index)
        if (strcmp(archon_generated_target_facts[index].name, name) == 0)
            return archon_generated_target_facts[index].opcode;
    return 0u;
}

static uint32_t zero_opcode(void) {
    size_t index;
    for (index = 0u; index < archon_generated_rewrite_rule_count; ++index) {
        const ArchonRewriteRule *rule =
            &archon_generated_rewrite_rules[index];
        if (rule->kind == ARCHON_REWRITE_RULE_IDENTITY
                && rule->constant_value == UINT64_C(0))
            return rule->constant_opcode;
    }
    return 0u;
}

int main(void) {
    struct Graph graph;
    ArchonGraphsAPI api;
    ArchonGraphsAPI owned_api;
    ArchonRewriteGraph owned;
    ArchonGraphsNodeId x, zero, add, result, children[3], leaves[3];
    ArchonRewriteStats stats;
    ArchonRewriteLimits limits;
    ArchonRewriteRule loop[2];

    memset(&graph, 0, sizeof(graph));
    api.context = &graph;
    api.read = read_node;
    api.intern = intern_node;
    api.node_count = node_count;
    api.cost = NULL;
    assert(ARCHON_IR_OP_RISCV32_ADD == fact_opcode("ADD"));

    assert(archon_rewrite_program(&api, ARCHON_REWRITE_DIRECTED, NULL,
                                  &result, &stats) == ARCHON_REWRITE_OK);
    assert(result == 0u);

    memset(&graph, 0, sizeof(graph));
    assert(intern_node(&graph, 7u, 3u, 0u, NULL, 0u, &x) == 0);
    assert(intern_node(&graph, zero_opcode(), 0u,
        ARCHON_GRAPHS_FLAG_CONSTANT, NULL, 0u, &zero) == 0);
    children[0] = x;
    children[1] = zero;
    assert(intern_node(&graph, fact_opcode("ADD"), 0u, 0u,
                       children, 2u, &add) == 0);
    assert(archon_rewrite_run_generated(&api, add, ARCHON_REWRITE_DIRECTED,
        NULL, &result, &stats) == ARCHON_REWRITE_OK);
    assert(result == x && stats.applications >= 2u && stats.saturated);
    limits = archon_rewrite_default_limits();
    limits.max_iterations = 1u;
    assert(archon_rewrite_run_generated(
        &api, add, ARCHON_REWRITE_EQUALITY_SATURATION,
        &limits, &result, &stats) == ARCHON_REWRITE_RESOURCE_LIMIT);
    assert(!stats.saturated);
    assert(archon_rewrite_run_generated(
        &api, add, ARCHON_REWRITE_EQUALITY_SATURATION,
        NULL, &result, &stats) == ARCHON_REWRITE_OK);
    assert(result == x && stats.saturated && stats.enodes >= 3u);
    children[0] = x;
    children[1] = x;
    assert(intern_node(&graph, fact_opcode("ADD"), 0u, 0u,
                       children, 2u, &add) == 0);
    assert(archon_rewrite_run_generated(
        &api, add, ARCHON_REWRITE_EQUALITY_SATURATION,
        NULL, &result, &stats) == ARCHON_REWRITE_OK);

    /*
     * Equal-cost recursive representatives must not displace the first
     * finite, acyclic representative during extraction.
     */
    memset(&graph, 0, sizeof(graph));
    assert(intern_node(&graph, 8u, 0u, 0u, NULL, 0u, &leaves[0]) == 0);
    assert(intern_node(&graph, 9u, 0u, 0u, NULL, 0u, &leaves[1]) == 0);
    assert(intern_node(&graph, 10u, 0u, 0u, NULL, 0u, &leaves[2]) == 0);
    assert(intern_node(&graph, 7u, 0u, 0u, leaves, 3u, &x) == 0);
    assert(intern_node(&graph, zero_opcode(), 0u,
        ARCHON_GRAPHS_FLAG_CONSTANT, NULL, 0u, &zero) == 0);
    children[0] = x;
    children[1] = zero;
    assert(intern_node(&graph, fact_opcode("ADD"), 0u, 0u,
                       children, 2u, &add) == 0);
    api.cost = tie_cost;
    assert(archon_rewrite_run_generated(
        &api, add, ARCHON_REWRITE_EQUALITY_SATURATION,
        NULL, &result, &stats) == ARCHON_REWRITE_OK);
    assert(result == x);
    api.cost = NULL;

    assert(intern_node(&graph, 7u, 3u, 0u, NULL, 0u, &x) == 0);
    memset(loop, 0, sizeof(loop));
    loop[0].name = "a-to-b";
    loop[0].kind = ARCHON_REWRITE_RULE_RENAME;
    loop[0].lhs_opcode = 7u;
    loop[0].rhs_opcode = 8u;
    loop[0].engines = ARCHON_REWRITE_DIRECTED;
    loop[1] = loop[0];
    loop[1].name = "b-to-a";
    loop[1].lhs_opcode = 8u;
    loop[1].rhs_opcode = 7u;
    limits = archon_rewrite_default_limits();
    limits.max_applications = 3u;
    assert(archon_rewrite_run(&api, x, ARCHON_REWRITE_DIRECTED,
        loop, 2u, &limits, &result, &stats)
        == ARCHON_REWRITE_RESOURCE_LIMIT);

    assert(archon_rewrite_graph_init(&owned) == ARCHON_REWRITE_OK);
    owned_api = archon_rewrite_graph_api(&owned);
    assert(archon_rewrite_graph_intern(
        &owned, 7u, 3u, 0u, NULL, 0u, &x) == ARCHON_REWRITE_OK);
    assert(archon_rewrite_graph_intern(
        &owned, zero_opcode(), 0u, ARCHON_GRAPHS_FLAG_CONSTANT,
        NULL, 0u, &zero) == ARCHON_REWRITE_OK);
    children[0] = x;
    children[1] = zero;
    assert(archon_rewrite_graph_intern(
        &owned, fact_opcode("ADD"), 0u, 0u, children, 2u, &add)
        == ARCHON_REWRITE_OK);
    assert(archon_graph_validate(&owned.dfg, NULL, 0u) == ARCHON_GRAPH_OK);
    assert(archon_rewrite_run_generated(
        &owned_api, add, ARCHON_REWRITE_EQUALITY_SATURATION,
        NULL, &result, &stats) == ARCHON_REWRITE_OK);
    assert(result == x);
    archon_rewrite_graph_dispose(&owned);
    return 0;
}
"""
        self.compile_and_run(
            files, test, defines=("ARCHON_REWRITE_INCLUDE_PROGRAM",)
        )


if __name__ == "__main__":
    unittest.main()
