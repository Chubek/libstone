"""Compile and execute the generated adapter-independent C99 analyses."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.irgen.analysis import emit


_HARNESS = r"""
#include "Analysis.h"
#include "Analysis.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct EdgeTable {
    const ArchonAnalysisId *sources;
    const ArchonAnalysisId *targets;
    size_t count;
} EdgeTable;

static ArchonAnalysisStatus edge_at(
    void *opaque, size_t index, ArchonAnalysisId *source,
    ArchonAnalysisId *target) {
    const EdgeTable *table = (const EdgeTable *)opaque;
    if (index >= table->count)
        return ARCHON_ANALYSIS_CALLBACK_FAILED;
    *source = table->sources[index];
    *target = table->targets[index];
    return ARCHON_ANALYSIS_OK;
}

static void test_cfg(void) {
    static const ArchonAnalysisId sources[] = {0, 1, 2, 1, 4};
    static const ArchonAnalysisId targets[] = {1, 2, 1, 3, 4};
    EdgeTable table = {sources, targets, 5};
    ArchonAnalysisGraphView graph = {
        5, 5, 0, NULL, NULL, edge_at, &table
    };
    unsigned char reachable[5];
    ArchonAnalysisId rpo[5];
    ArchonAnalysisId idom[5];
    ArchonAnalysisId components[5];
    ArchonAnalysisId loop_by_node[5];
    size_t sizes[5];
    size_t count;
    size_t component_count;
    ArchonAnalysisLoop loops[5];
    ArchonAnalysisError error;
    int answer = 0;

    assert(archon_analysis_reachability(
        &graph, 0, reachable, 5, &count, NULL, &error) ==
        ARCHON_ANALYSIS_OK);
    assert(count == 4);
    assert(reachable[0] && reachable[1] && reachable[2] && reachable[3]);
    assert(!reachable[4]);

    assert(archon_analysis_reverse_postorder(
        &graph, 0, rpo, 5, &count, NULL, &error) ==
        ARCHON_ANALYSIS_OK);
    assert(count == 4 && rpo[0] == 0 && rpo[1] == 1);

    assert(archon_analysis_dominators(
        &graph, 0, idom, 5, NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(idom[0] == 0 && idom[1] == 0);
    assert(idom[2] == 1 && idom[3] == 1);
    assert(idom[4] == ARCHON_ANALYSIS_INVALID_ID);
    assert(archon_analysis_dominates(
        idom, 5, 1, 3, &answer, &error) == ARCHON_ANALYSIS_OK);
    assert(answer);
    assert(archon_analysis_dominates(
        idom, 5, 2, 3, &answer, &error) == ARCHON_ANALYSIS_OK);
    assert(!answer);

    assert(archon_analysis_strong_components(
        &graph, components, 5, sizes, 5, &component_count,
        NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(component_count == 4);
    assert(components[1] == components[2]);

    assert(archon_analysis_loops(
        &graph, loops, 5, loop_by_node, 5, &count,
        NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(count == 2); /* reachable 1<->2 and unreachable self-loop 4 */
    assert(loop_by_node[1] == loop_by_node[2]);
    assert(loop_by_node[0] == ARCHON_ANALYSIS_INVALID_ID);
    assert(loops[loop_by_node[1]].header == 1);
    assert(loops[loop_by_node[1]].reducible);

    {
        ArchonAnalysisConfig limits;
        archon_analysis_config_init(&limits);
        limits.max_nodes = 2;
        assert(archon_analysis_reachability(
            &graph, 0, reachable, 5, &count, &limits, &error) ==
            ARCHON_ANALYSIS_LIMIT_EXCEEDED);
        assert(strstr(error.message, "too many nodes") != NULL);
    }
}

static void test_use_def_and_liveness(void) {
    static const ArchonAnalysisId sources[] = {0, 1, 2, 1, 4};
    static const ArchonAnalysisId targets[] = {1, 2, 1, 3, 4};
    ArchonAnalysisGraphView graph = {
        5, 5, 0, sources, targets, NULL, NULL
    };
    ArchonAnalysisDefinition definitions[] = {
        {0, 0, 0, 1, ARCHON_ANALYSIS_DEF_NONE},
        {1, 1, 1, 11, ARCHON_ANALYSIS_DEF_NONE},
        {2, 2, 1, 12, ARCHON_ANALYSIS_DEF_NONE}
    };
    ArchonAnalysisUse uses[] = {
        {0, 1, 1, 10, ARCHON_ANALYSIS_INVALID_ID,
         ARCHON_ANALYSIS_USE_NONE},
        {1, 2, 2, 21, ARCHON_ANALYSIS_INVALID_ID,
         ARCHON_ANALYSIS_USE_NONE},
        {1, 3, 1, 13, 2, ARCHON_ANALYSIS_USE_EDGE}
    };
    ArchonAnalysisProgramView program = {
        5, 4, 3, 3, 3, definitions, uses, NULL, NULL, NULL
    };
    ArchonAnalysisId definition_map[3];
    ArchonAnalysisBlockValue block_definitions[] = {
        {0, 0}, {1, 1}, {1, 2}
    };
    ArchonAnalysisBlockValue block_uses[] = {
        {1, 0}, {2, 1}, {3, 1}
    };
    ArchonAnalysisEdgeUse edge_uses[] = {
        {1, 3, 2}
    };
    ArchonAnalysisLivenessView liveness = {
        5, 3, 3, 3, 1,
        block_definitions, block_uses, edge_uses,
        NULL, NULL, NULL, NULL
    };
    uint64_t live_in[5];
    uint64_t live_out[5];
    size_t words = 0;
    ArchonAnalysisError error;

    assert(archon_analysis_validate_use_def(
        &graph, &program, definition_map, 3, NULL, &error) ==
        ARCHON_ANALYSIS_OK);
    assert(definition_map[0] == 0 && definition_map[2] == 2);

    assert(archon_analysis_liveness(
        &graph, &liveness, live_in, 5, live_out, 5, &words,
        NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(words == 5);
    assert(archon_analysis_liveness_test(live_in, 5, 3, 1, 0));
    assert(!archon_analysis_liveness_test(live_in, 5, 3, 1, 2));
    assert(archon_analysis_liveness_test(live_out, 5, 3, 1, 2));
    assert(!archon_analysis_liveness_test(live_in, 5, 3, 3, 2));

    uses[2].predecessor = 3;
    assert(archon_analysis_validate_use_def(
        &graph, &program, NULL, 0, NULL, &error) ==
        ARCHON_ANALYSIS_INVALID_PROGRAM);
    assert(strstr(error.message, "predecessor edge") != NULL);
}

static void test_intervals_and_pressure(void) {
    ArchonAnalysisBlockRange blocks[] = {
        {0, 0, 10}, {1, 10, 20}, {2, 30, 40},
        {3, 50, 60}, {4, 70, 80}
    };
    ArchonAnalysisValueEvent events[] = {
        {0, 0, 2, ARCHON_ANALYSIS_EVENT_DEF},
        {2, 0, 32, ARCHON_ANALYSIS_EVENT_USE},
        {1, 1, 12, ARCHON_ANALYSIS_EVENT_DEF},
        {1, 1, 15, ARCHON_ANALYSIS_EVENT_USE},
        {1, 2, 14, ARCHON_ANALYSIS_EVENT_DEF}
    };
    ArchonAnalysisIntervalView interval_view = {
        5, 3, 5, blocks, events, NULL, NULL, NULL
    };
    uint64_t empty_liveness[5] = {0, 0, 0, 0, 0};
    ArchonAnalysisLiveInterval intervals[3];
    ArchonAnalysisLiveSegment segments[4];
    ArchonAnalysisPressureValue values[] = {
        {0, 1}, {0, 1}, {0, 2}
    };
    ArchonAnalysisPressureView pressure_view = {
        3, 1, values, NULL, NULL
    };
    ArchonAnalysisPressurePeak peaks[1];
    ArchonAnalysisError error;
    size_t interval_count;
    size_t segment_count;
    size_t peak_count;

    assert(archon_analysis_live_intervals(
        &interval_view, empty_liveness, empty_liveness, 5,
        intervals, 3, segments, 4, &interval_count, &segment_count,
        NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(interval_count == 3 && segment_count == 4);
    assert(intervals[0].segment_count == 2);
    assert(segments[0].begin == 2 && segments[0].end == 3);
    assert(segments[1].begin == 32 && segments[1].end == 33);
    assert(intervals[1].segment_count == 1);
    assert(segments[2].begin == 12 && segments[2].end == 16);

    assert(archon_analysis_register_pressure(
        &pressure_view, intervals, interval_count, segments, segment_count,
        peaks, 1, &peak_count, NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(peak_count == 1);
    assert(peaks[0].pressure == 3 && peaks[0].position == 14);
}

static void test_dependencies_and_critical_paths(void) {
    ArchonAnalysisAccess accesses[] = {
        {0, 0, ARCHON_ANALYSIS_ACCESS_WRITE},
        {1, 0, ARCHON_ANALYSIS_ACCESS_READ},
        {1, 1, ARCHON_ANALYSIS_ACCESS_WRITE},
        {2, 0, ARCHON_ANALYSIS_ACCESS_READ},
        {2, 1, ARCHON_ANALYSIS_ACCESS_READ},
        {3, 0, ARCHON_ANALYSIS_ACCESS_WRITE}
    };
    ArchonAnalysisDependency explicit_dependencies[] = {
        {2, 3, ARCHON_ANALYSIS_INVALID_ID,
         ARCHON_ANALYSIS_DEP_ORDER, 4}
    };
    uint64_t latencies[] = {2, 3, 1, 1};
    ArchonAnalysisDependencyInput input = {
        4, 2, 6, 1, accesses, explicit_dependencies, latencies,
        NULL, NULL, NULL, NULL
    };
    ArchonAnalysisDependency dependencies[16];
    ArchonAnalysisDependencyView dependency_view;
    ArchonAnalysisCriticalPathBuffers buffers;
    uint64_t earliest[4];
    uint64_t path_length;
    ArchonAnalysisId predecessor[4];
    ArchonAnalysisId topological[4];
    ArchonAnalysisError error;
    size_t dependency_count;

    assert(archon_analysis_dependency_dag(
        &input, dependencies, 16, &dependency_count,
        NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(dependency_count == 7);

    dependency_view.node_count = 4;
    dependency_view.dependency_count = dependency_count;
    dependency_view.dependencies = dependencies;
    dependency_view.node_latencies = latencies;
    dependency_view.dependency_at = NULL;
    dependency_view.latency_at = NULL;
    dependency_view.context = NULL;
    buffers.capacity = 4;
    buffers.earliest_start = earliest;
    buffers.predecessor = predecessor;
    buffers.topological_order = topological;

    assert(archon_analysis_critical_paths(
        &dependency_view, &buffers, &path_length,
        NULL, &error) == ARCHON_ANALYSIS_OK);
    assert(topological[0] == 0 && topological[1] == 1);
    assert(topological[2] == 2 && topological[3] == 3);
    assert(earliest[0] == 0 && earliest[1] == 2);
    assert(earliest[2] == 5 && earliest[3] == 9);
    assert(predecessor[3] == 2 && path_length == 10);
}

int main(void) {
    test_cfg();
    test_use_def_and_liveness();
    test_intervals_and_pressure();
    test_dependencies_and_critical_paths();
    puts("analysis-ok");
    return 0;
}
"""


class GeneratedAnalysisTests(unittest.TestCase):
    def test_emit_is_deterministic_and_context_free(self):
        first = emit({})
        second = emit({"architectures": [{"architecture": "ignored"}]})
        self.assertEqual(first, second)
        self.assertEqual(set(first), {"Analysis.c", "Analysis.h"})
        self.assertNotIn("Graphs.h", first["Analysis.h"])
        self.assertIn("archon_analysis_live_intervals", first["Analysis.h"])

    def test_gcc_and_clang_compile_and_run_strict_c99(self):
        compilers = [
            compiler for compiler in ("gcc", "clang")
            if shutil.which(compiler)
        ]
        if not compilers:
            self.skipTest("GCC and Clang are unavailable")

        outputs = emit({})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in outputs.items():
                (root / name).write_text(text, encoding="ascii")
            (root / "test.c").write_text(_HARNESS, encoding="ascii")

            for compiler in compilers:
                executable = root / f"analysis-{compiler}"
                with self.subTest(compiler=compiler):
                    subprocess.run(
                        [
                            compiler,
                            "-std=c99",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-pedantic",
                            "Analysis.c",
                            "test.c",
                            "-o",
                            str(executable),
                        ],
                        cwd=root,
                        check=True,
                    )
                    completed = subprocess.run(
                        [str(executable)],
                        cwd=root,
                        check=True,
                        text=True,
                        stdout=subprocess.PIPE,
                    )
                    self.assertEqual(completed.stdout, "analysis-ok\n")


if __name__ == "__main__":
    unittest.main()
