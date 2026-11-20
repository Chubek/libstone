"""Compile and exercise the generated, dependency-free C99 support layer."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.irgen import support


_HARNESS = r"""#include "Support.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void count_diagnostic(void *user, const ArchonDiagnostic *diagnostic) {
    size_t *count = (size_t *)user;
    assert(diagnostic != NULL);
    ++*count;
}

static void test_checked_allocation(void) {
    ArchonAllocator allocator;
    ArchonFaultAllocator fault;
    void *memory = NULL;
    size_t result = 0u;

    archon_allocator_default(&allocator);
    assert(archon_allocator_valid(&allocator));
    assert(archon_size_add(4u, 5u, &result) && result == 9u);
    assert(!archon_size_add((size_t)-1, 1u, &result));
    assert(archon_size_mul(4u, 5u, &result) && result == 20u);
    assert(!archon_size_mul((size_t)-1, 2u, &result));
    assert(archon_allocate_array(
               &allocator, (size_t)-1, 2u, &memory) ==
           ARCHON_SUPPORT_OVERFLOW);
    assert(memory == NULL);

    assert(archon_fault_allocator_init(&fault, &allocator) ==
           ARCHON_SUPPORT_OK);
    archon_fault_allocator_fail_after(&fault, 0u);
    assert(archon_allocate_array(
               &fault.allocator, 1u, 16u, &memory) ==
           ARCHON_SUPPORT_OUT_OF_MEMORY);
    assert(memory == NULL && archon_fault_allocator_attempts(&fault) == 1u);
    archon_fault_allocator_disable(&fault);
    assert(archon_allocate_array(
               &fault.allocator, 4u, sizeof(uint32_t), &memory) ==
           ARCHON_SUPPORT_OK);
    assert(memory != NULL);
    archon_deallocate(&fault.allocator, memory);
}

static void test_arena_and_vector(void) {
    ArchonArena arena;
    ArchonArenaMark mark;
    ArchonVector vector;
    ArchonFaultAllocator fault;
    void *first = NULL;
    void *second = NULL;
    int value;
    size_t used;
    size_t index;

    assert(archon_arena_init(&arena, NULL, 32u) == ARCHON_SUPPORT_OK);
    assert(archon_arena_alloc(&arena, 7u, 8u, &first) ==
           ARCHON_SUPPORT_OK);
    assert(first != NULL && ((uintptr_t)first & (uintptr_t)7u) == 0u);
    mark = archon_arena_mark(&arena);
    used = archon_arena_bytes_used(&arena);
    assert(archon_arena_alloc(&arena, 100u, 16u, &second) ==
           ARCHON_SUPPORT_OK);
    assert(second != NULL && second != first);
    assert(archon_arena_rewind(&arena, mark) == ARCHON_SUPPORT_OK);
    assert(archon_arena_bytes_used(&arena) == used);
    assert(archon_arena_alloc_array(
               &arena, (size_t)-1, 2u, 1u, &second) ==
           ARCHON_SUPPORT_OVERFLOW);
    archon_arena_clear(&arena);
    assert(archon_arena_rewind(&arena, mark) ==
           ARCHON_SUPPORT_INVALID_ARGUMENT);
    archon_arena_dispose(&arena);

    assert(archon_fault_allocator_init(&fault, NULL) == ARCHON_SUPPORT_OK);
    assert(archon_vector_init(
               &vector, sizeof(int), &fault.allocator) ==
           ARCHON_SUPPORT_OK);
    for (index = 0u; index < 8u; ++index) {
        value = (int)index;
        assert(archon_vector_push(&vector, &value, NULL) ==
               ARCHON_SUPPORT_OK);
    }
    archon_fault_allocator_fail_after(&fault, 0u);
    value = 99;
    assert(archon_vector_push(&vector, &value, NULL) ==
           ARCHON_SUPPORT_OUT_OF_MEMORY);
    assert(vector.length == 8u);
    archon_fault_allocator_disable(&fault);
    assert(archon_vector_insert(&vector, 1u, &value) ==
           ARCHON_SUPPORT_OK);
    assert(*(int *)archon_vector_at(&vector, 1u) == 99);
    assert(archon_vector_remove(&vector, 1u, 1u) ==
           ARCHON_SUPPORT_OK);
    assert(*(const int *)archon_vector_at_const(&vector, 1u) == 1);
    assert(archon_vector_resize(&vector, 10u) == ARCHON_SUPPORT_OK);
    assert(*(const int *)archon_vector_at_const(&vector, 9u) == 0);
    archon_vector_dispose(&vector);
}

static void test_integer_collections(void) {
    ArchonIntMap map;
    ArchonIntMapIterator iterator;
    ArchonIntSet set;
    ArchonIntSetIterator set_iterator;
    ArchonFaultAllocator fault;
    uint64_t key;
    uint64_t value;
    int changed;

    assert(archon_int_map_init(&map, NULL) == ARCHON_SUPPORT_OK);
    assert(archon_int_map_put(&map, 3u, 30u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_map_put(&map, 1u, 10u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_map_put(&map, 2u, 20u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_map_put(&map, 1u, 11u, &changed) ==
           ARCHON_SUPPORT_OK && !changed);
    assert(archon_int_map_get(&map, 1u, &value) ==
           ARCHON_SUPPORT_OK && value == 11u);
    assert(archon_int_map_remove(&map, 1u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_map_put(&map, 1u, 12u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_map_count(&map) == 3u);
    archon_int_map_iterator_init(&iterator);
    assert(archon_int_map_iterator_next(&map, &iterator, &key, &value));
    assert(key == 3u && value == 30u);
    assert(archon_int_map_iterator_next(&map, &iterator, &key, &value));
    assert(key == 2u && value == 20u);
    assert(archon_int_map_iterator_next(&map, &iterator, &key, &value));
    assert(key == 1u && value == 12u);
    assert(!archon_int_map_iterator_next(&map, &iterator, &key, &value));
    archon_int_map_dispose(&map);

    assert(archon_int_set_init(&set, NULL) == ARCHON_SUPPORT_OK);
    assert(archon_int_set_add(&set, 9u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_set_add(&set, 7u, &changed) ==
           ARCHON_SUPPORT_OK && changed);
    assert(archon_int_set_add(&set, 9u, &changed) ==
           ARCHON_SUPPORT_OK && !changed);
    assert(archon_int_set_contains(&set, 7u));
    archon_int_set_iterator_init(&set_iterator);
    assert(archon_int_set_iterator_next(&set, &set_iterator, &value));
    assert(value == 9u);
    assert(archon_int_set_iterator_next(&set, &set_iterator, &value));
    assert(value == 7u);
    assert(!archon_int_set_iterator_next(&set, &set_iterator, &value));
    archon_int_set_dispose(&set);

    assert(archon_fault_allocator_init(&fault, NULL) == ARCHON_SUPPORT_OK);
    assert(archon_int_map_init(&map, &fault.allocator) ==
           ARCHON_SUPPORT_OK);
    archon_fault_allocator_fail_after(&fault, 0u);
    assert(archon_int_map_put(&map, 1u, 1u, NULL) ==
           ARCHON_SUPPORT_OUT_OF_MEMORY);
    assert(archon_int_map_count(&map) == 0u);
    archon_int_map_dispose(&map);
}

static void test_strings_and_diagnostics(void) {
    ArchonStringPool pool;
    ArchonStringId first;
    ArchonStringId again;
    ArchonStringId binary;
    ArchonDiagnostics diagnostics;
    const ArchonDiagnostic *diagnostic;
    const char embedded[] = {'a', '\0', 'b'};
    size_t length;
    size_t sink_count = 0u;
    FILE *stream;

    assert(archon_hash_bytes("abc", 3u) ==
           UINT64_C(0xe71fa2190541574b));
    assert(archon_hash_string("abc") == archon_hash_bytes("abc", 3u));

    assert(archon_string_pool_init(&pool, NULL) == ARCHON_SUPPORT_OK);
    assert(archon_string_pool_intern(&pool, "alpha", &first) ==
           ARCHON_SUPPORT_OK);
    assert(archon_string_pool_intern(&pool, "alpha", &again) ==
           ARCHON_SUPPORT_OK && first == again);
    assert(archon_string_pool_intern_n(
               &pool, embedded, sizeof embedded, &binary) ==
           ARCHON_SUPPORT_OK);
    assert(binary != first && archon_string_pool_count(&pool) == 2u);
    assert(memcmp(
               archon_string_pool_get(&pool, binary, &length),
               embedded, sizeof embedded) == 0);
    assert(length == sizeof embedded);
    archon_string_pool_dispose(&pool);

    assert(archon_diagnostics_init(
               &diagnostics, NULL, count_diagnostic, &sink_count) ==
           ARCHON_SUPPORT_OK);
    assert(archon_diagnostics_add(
               &diagnostics, ARCHON_DIAGNOSTIC_WARNING,
               "input.arch", 2u, 3u, "warning text") ==
           ARCHON_SUPPORT_OK);
    assert(archon_diagnostics_addf(
               &diagnostics, ARCHON_DIAGNOSTIC_ERROR,
               "input.arch", 4u, 5u, "bad value %d", 17) ==
           ARCHON_SUPPORT_OK);
    assert(archon_diagnostics_count(&diagnostics) == 2u);
    assert(archon_diagnostics_error_count(&diagnostics) == 1u);
    assert(sink_count == 2u);
    diagnostic = archon_diagnostics_get(&diagnostics, 1u);
    assert(diagnostic != NULL &&
           strcmp(diagnostic->message, "bad value 17") == 0);
    stream = tmpfile();
    assert(stream != NULL);
    assert(archon_diagnostics_write(&diagnostics, stream) ==
           ARCHON_SUPPORT_OK);
    assert(fclose(stream) == 0);
    archon_diagnostics_clear(&diagnostics);
    assert(archon_diagnostics_count(&diagnostics) == 0u);
    archon_diagnostics_dispose(&diagnostics);
}

static void test_files(void) {
    ArchonFileBuffer buffer;
    ArchonFaultAllocator fault;
    const char *path = "support-atomic-test.bin";

    assert(archon_file_write_all(path, "old", 3u) == ARCHON_SUPPORT_OK);
    assert(archon_file_write_atomic(path, "replacement", 11u, NULL) ==
           ARCHON_SUPPORT_OK);
    assert(archon_file_read_all(path, NULL, &buffer) ==
           ARCHON_SUPPORT_OK);
    assert(buffer.size == 11u && buffer.data[buffer.size] == 0u);
    assert(memcmp(buffer.data, "replacement", 11u) == 0);
    archon_file_buffer_dispose(&buffer);

    assert(archon_fault_allocator_init(&fault, NULL) == ARCHON_SUPPORT_OK);
    archon_fault_allocator_fail_after(&fault, 0u);
    assert(archon_file_write_atomic(
               path, "lost", 4u, &fault.allocator) ==
           ARCHON_SUPPORT_OUT_OF_MEMORY);
    assert(archon_file_read_all(path, NULL, &buffer) ==
           ARCHON_SUPPORT_OK);
    assert(buffer.size == 11u &&
           memcmp(buffer.data, "replacement", 11u) == 0);
    archon_file_buffer_dispose(&buffer);
    assert(remove(path) == 0);
}

int main(void) {
    assert(strcmp(
               archon_support_status_name(ARCHON_SUPPORT_OVERFLOW),
               "overflow") == 0);
    test_checked_allocation();
    test_arena_and_vector();
    test_integer_collections();
    test_strings_and_diagnostics();
    test_files();
    return 0;
}
"""


class IRGeneratorSupportTests(unittest.TestCase):
    def compilers(self):
        compilers = [
            name for name in ("gcc", "clang") if shutil.which(name)
        ]
        if not compilers:
            self.skipTest("no strict C99 compiler is available")
        return compilers

    def test_emit_is_context_independent_and_test_hooks_are_gated(self):
        first = support.emit(None)
        second = support.emit({"architectures": [{"architecture": "ignored"}]})
        self.assertEqual(first, second)
        self.assertEqual(set(first), {"Support.c", "Support.h"})
        self.assertIn("#ifdef ARCHON_SUPPORT_TESTING", first["Support.h"])
        self.assertNotIn("static ArchonFaultAllocator", first["Support.c"])
        for text in first.values():
            text.encode("ascii")

    def test_strict_c99_gcc_and_clang_with_failures_and_overflow(self):
        outputs = support.emit(None)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in outputs.items():
                (root / name).write_text(text, encoding="ascii")
            (root / "test.c").write_text(_HARNESS, encoding="ascii")

            for compiler in self.compilers():
                executable = root / f"test-{Path(compiler).name}"
                with self.subTest(compiler=compiler):
                    subprocess.run(
                        [
                            compiler,
                            "-DARCHON_SUPPORT_TESTING",
                            "-std=c99",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-pedantic",
                            "Support.c",
                            "test.c",
                            "-o",
                            str(executable),
                        ],
                        cwd=root,
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                    subprocess.run(
                        [str(executable)],
                        cwd=root,
                        check=True,
                        capture_output=True,
                        text=True,
                    )

                    subprocess.run(
                        [
                            compiler,
                            "-std=c99",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            "-pedantic",
                            "-c",
                            "Support.c",
                            "-o",
                            str(root / f"production-{Path(compiler).name}.o"),
                        ],
                        cwd=root,
                        check=True,
                        capture_output=True,
                        text=True,
                    )


if __name__ == "__main__":
    unittest.main()
