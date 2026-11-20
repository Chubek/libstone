"""Compile-test the generated, opt-in C99 tracing/profiling layer.

Integration is intentionally tested without changes to Archon's driver: the
generator stage emits Trace.c/Trace.h, and pipeline implementations opt in by
including the header and using its macros.  Both GCC and Clang are exercised
when installed.  Disabled tests deliberately do not link Trace.c, proving that
instrumentation vanishes as well as avoiding argument evaluation.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.irgen.trace import emit


_ENABLED_PROGRAM = r'''
#include "Trace.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Buffer {
    char bytes[32768];
    size_t length;
    int calls;
    int fail_call;
    int failure;
};

struct TestClock {
    const uint64_t *ticks;
    size_t count;
    size_t index;
    int fail_index;
    int failure;
};

static void buffer_init(struct Buffer *buffer) {
    buffer->length = 0u;
    buffer->calls = 0;
    buffer->fail_call = -1;
    buffer->failure = 0;
    buffer->bytes[0] = '\0';
}

static int write_buffer(void *context, const char *bytes, size_t length) {
    struct Buffer *buffer = (struct Buffer *)context;
    if (buffer->fail_call >= 0 && buffer->calls == buffer->fail_call) {
        ++buffer->calls;
        return buffer->failure;
    }
    ++buffer->calls;
    if (length > sizeof buffer->bytes - 1u - buffer->length)
        return 99;
    memcpy(buffer->bytes + buffer->length, bytes, length);
    buffer->length += length;
    buffer->bytes[buffer->length] = '\0';
    return 0;
}

static int read_clock(void *context, uint64_t *tick) {
    struct TestClock *clock = (struct TestClock *)context;
    if (clock->fail_index >= 0 &&
            clock->index == (size_t)clock->fail_index) {
        ++clock->index;
        return clock->failure;
    }
    if (clock->index >= clock->count)
        return 98;
    *tick = clock->ticks[clock->index++];
    return 0;
}

static void run_records(struct Buffer *output, ArchonTraceFormat format) {
    static const uint64_t ticks[] = {
        UINT64_C(10), UINT64_C(11), UINT64_C(12), UINT64_C(13),
        UINT64_C(14), UINT64_C(15), UINT64_C(20), UINT64_C(25)
    };
    static const char outer_name[] =
        "outer\"\\\n\t" "\001" "\xc3\xa9";
    struct TestClock clock = {
        ticks, sizeof ticks / sizeof ticks[0], 0u, -1, 0
    };
    ArchonTrace trace;
    ArchonTraceStage outer;
    ArchonTraceStage inner;
    ArchonTraceMemoryCounters memory = {
        UINT64_C(12), UINT64_C(64), UINT64_C(80),
        UINT64_C(68), UINT64_C(5), UINT64_C(3)
    };

    buffer_init(output);
    ARCHON_TRACE_INIT(
        &trace, format, write_buffer, output, read_clock, &clock);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_STAGE_BEGIN(&trace, outer_name, &outer);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_COUNTER(&trace, "nodes\nvisited", UINT64_C(17));
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_STAGE_BEGIN(&trace, "inner\\stage", &inner);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_SOLVER_ITERATION(
        &trace, "smt\"solver", UINT64_C(2), UINT64_C(9), UINT64_C(4));
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_REWRITE_ITERATION(
        &trace, "canonical", UINT64_C(3), UINT64_C(7), UINT64_C(6));
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_MEMORY(&trace, "arena\tzero", &memory);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_STAGE_END(&trace, &inner, -7);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_STAGE_END(&trace, &outer, 0);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    ARCHON_TRACE_FINISH(&trace);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_OK);
    assert(archon_trace_event_count(&trace) == UINT64_C(8));
    assert(archon_trace_depth(&trace) == UINT64_C(0));
    assert(clock.index == clock.count);
}

static void test_error_propagation(void) {
    static const uint64_t ticks[] = {UINT64_C(1), UINT64_C(2)};
    struct TestClock clock = {ticks, 2u, 0u, -1, 0};
    struct Buffer output;
    ArchonTrace trace;
    size_t calls;

    buffer_init(&output);
    output.fail_call = 0;
    output.failure = 73;
    assert(archon_trace_init(
        &trace, ARCHON_TRACE_JSON_LINES, write_buffer, &output,
        read_clock, &clock
    ) == ARCHON_TRACE_OK);
    assert(archon_trace_counter(
        &trace, "fails", UINT64_C(1)
    ) == ARCHON_TRACE_WRITE_ERROR);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_WRITE_ERROR);
    assert(archon_trace_callback_error(&trace) == 73);
    assert(archon_trace_event_count(&trace) == UINT64_C(0));
    calls = clock.index;
    assert(archon_trace_counter(
        &trace, "not-called", UINT64_C(2)
    ) == ARCHON_TRACE_WRITE_ERROR);
    assert(clock.index == calls);

    buffer_init(&output);
    clock.index = 0u;
    clock.fail_index = 0;
    clock.failure = 81;
    assert(archon_trace_init(
        &trace, ARCHON_TRACE_TEXT, write_buffer, &output,
        read_clock, &clock
    ) == ARCHON_TRACE_OK);
    assert(archon_trace_counter(
        &trace, "clock-fails", UINT64_C(1)
    ) == ARCHON_TRACE_CLOCK_ERROR);
    assert(archon_trace_callback_error(&trace) == 81);
    assert(output.length == 0u);
}

static void test_nesting_errors(void) {
    static const uint64_t ticks[] = {UINT64_C(1), UINT64_C(2)};
    struct TestClock clock = {ticks, 2u, 0u, -1, 0};
    struct Buffer output;
    ArchonTrace trace;
    ArchonTraceStage outer;
    ArchonTraceStage inner;

    buffer_init(&output);
    assert(archon_trace_init(
        &trace, ARCHON_TRACE_TEXT, write_buffer, &output,
        read_clock, &clock
    ) == ARCHON_TRACE_OK);
    assert(archon_trace_stage_begin(
        &trace, "outer", &outer
    ) == ARCHON_TRACE_OK);
    assert(archon_trace_stage_begin(
        &trace, "inner", &inner
    ) == ARCHON_TRACE_OK);
    assert(archon_trace_stage_end(
        &trace, &outer, 0
    ) == ARCHON_TRACE_STAGE_MISMATCH);
    assert(archon_trace_status(&trace) == ARCHON_TRACE_STAGE_MISMATCH);
}

int main(void) {
    struct Buffer first;
    struct Buffer second;
    struct Buffer text;

    run_records(&first, ARCHON_TRACE_JSON_LINES);
    run_records(&second, ARCHON_TRACE_JSON_LINES);
    assert(first.length == second.length);
    assert(memcmp(first.bytes, second.bytes, first.length) == 0);

    run_records(&text, ARCHON_TRACE_TEXT);
    assert(strstr(text.bytes, "event=\"stage_begin\"") != NULL);
    assert(strstr(text.bytes, "\\\"") != NULL);
    assert(strstr(text.bytes, "\\\\") != NULL);
    assert(strstr(text.bytes, "\\n") != NULL);
    assert(strstr(text.bytes, "\\u0001") != NULL);
    assert(strstr(text.bytes, "duration=15") != NULL);

    test_error_propagation();
    test_nesting_errors();
    assert(fwrite(first.bytes, 1u, first.length, stdout) == first.length);
    return 0;
}
'''


_DISABLED_PROGRAM = r'''
#include "Trace.h"

#include <assert.h>

int main(void) {
    int effects = 0;

    ARCHON_TRACE_INIT(
        ++effects, ++effects, ++effects, ++effects, ++effects, ++effects);
    ARCHON_TRACE_STAGE_BEGIN(++effects, ++effects, ++effects);
    ARCHON_TRACE_COUNTER(++effects, ++effects, ++effects);
    ARCHON_TRACE_SOLVER_ITERATION(
        ++effects, ++effects, ++effects, ++effects, ++effects);
    ARCHON_TRACE_REWRITE_ITERATION(
        ++effects, ++effects, ++effects, ++effects, ++effects);
    ARCHON_TRACE_MEMORY(++effects, ++effects, ++effects);
    ARCHON_TRACE_STAGE_END(++effects, ++effects, ++effects);
    ARCHON_TRACE_FINISH(++effects);
    assert(effects == 0);
    return 0;
}
'''


class TraceGeneratorTests(unittest.TestCase):
    def test_emit_is_context_independent_and_self_contained(self):
        first = emit(None)
        second = emit({"architectures": [{"architecture": "ignored"}]})
        self.assertEqual(set(first), {"Trace.c", "Trace.h"})
        self.assertEqual(first, second)
        self.assertIn("ARCHON_TRACE_EVENT_STAGE_BEGIN = 1", first["Trace.h"])
        self.assertNotIn("#include <time.h>", first["Trace.c"])
        self.assertNotIn("#include <pthread.h>", first["Trace.c"])

    def _compiler(self, name: str) -> str:
        compiler = shutil.which(name)
        if compiler is None:
            self.skipTest(f"{name} is unavailable")
        return compiler

    def _compile_and_run(
        self, compiler_name: str, source: str, *, enabled: bool
    ) -> subprocess.CompletedProcess[str]:
        compiler = self._compiler(compiler_name)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outputs = emit({})
            (root / "Trace.h").write_text(outputs["Trace.h"], encoding="ascii")
            (root / "test.c").write_text(source, encoding="ascii")
            command = [
                compiler,
                "-std=c99",
                "-pedantic-errors",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-DARCHON_TRACE_ENABLED={int(enabled)}",
                "test.c",
            ]
            if enabled:
                (root / "Trace.c").write_text(outputs["Trace.c"], encoding="ascii")
                command.append("Trace.c")
            command.extend(["-o", "test"])
            subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
            return subprocess.run(
                [str(root / "test")],
                cwd=root,
                check=True,
                capture_output=True,
                text=True,
            )

    def _check_enabled(self, compiler: str) -> None:
        completed = self._compile_and_run(
            compiler, _ENABLED_PROGRAM, enabled=True
        )
        records = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertEqual([record["id"] for record in records], list(range(1, 9)))
        self.assertEqual(
            [record["kind"] for record in records],
            [1, 3, 1, 4, 5, 6, 2, 2],
        )
        self.assertEqual(
            [record["event"] for record in records],
            [
                "stage_begin",
                "counter",
                "stage_begin",
                "solver_iteration",
                "rewrite_iteration",
                "memory",
                "stage_end",
                "stage_end",
            ],
        )
        self.assertEqual(
            [record["depth"] for record in records],
            [0, 1, 1, 2, 2, 2, 1, 0],
        )
        self.assertEqual(
            [record["tick"] for record in records],
            [10, 11, 12, 13, 14, 15, 20, 25],
        )
        self.assertEqual(records[0]["name"], 'outer"\\\n\t\x01é')
        self.assertEqual(records[1]["name"], "nodes\nvisited")
        self.assertEqual(records[2]["name"], "inner\\stage")
        self.assertEqual(records[3]["name"], 'smt"solver')
        self.assertEqual(records[5]["name"], "arena\tzero")
        self.assertEqual(records[6]["duration"], 8)
        self.assertEqual(records[6]["result"], -7)
        self.assertEqual(records[7]["duration"], 15)
        self.assertEqual(records[0]["span"], records[7]["span"])
        self.assertEqual(records[0]["stage_id"], records[7]["stage_id"])
        self.assertEqual(records[0]["stage_id"], 15505249376559545946)
        self.assertEqual(records[2]["span"], records[6]["span"])
        self.assertEqual(records[4]["matches"], 7)
        self.assertEqual(records[4]["applied"], 6)
        self.assertEqual(records[5]["live_bytes"], 12)
        self.assertEqual(records[5]["peak_bytes"], 64)

    def test_enabled_gcc(self):
        self._check_enabled("gcc")

    def test_enabled_clang(self):
        self._check_enabled("clang")

    def test_disabled_gcc_erases_argument_evaluation_and_linkage(self):
        self._compile_and_run("gcc", _DISABLED_PROGRAM, enabled=False)

    def test_disabled_clang_erases_argument_evaluation_and_linkage(self):
        self._compile_and_run("clang", _DISABLED_PROGRAM, enabled=False)


if __name__ == "__main__":
    unittest.main()
