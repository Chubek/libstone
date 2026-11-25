"""Exercise Cephyr's native AMD64 and AArch64 C11 atomic subset."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


SEQUENCE = r"""
#include <stdatomic.h>
void access_sequence(volatile atomic_int *p) {
    *p;
    *p = 7;
    (void)sizeof *p;
    (void)sizeof atomic_load(p);
    (void)_Generic(*p, int: 0, default: 1);
    (void)&*p;
    (*p)++;
    atomic_thread_fence(memory_order_acquire);
    atomic_signal_fence(memory_order_release);
    (void)atomic_load_explicit(p, memory_order_relaxed);
}
"""

EXPECTED = ["load_4", "store_4", "fetch_add_4", "thread_fence", "signal_fence", "load_4"]
CALLS = re.compile(r'(?:call|bl)\s+"?__cephyr_rt_atomic_(\w+)')

PROGRAM = r"""
#include <stdatomic.h>
#if defined(__STDC_NO_ATOMICS__)
#error unexpectedly opted out of atomics
#endif
#if ATOMIC_INT_LOCK_FREE != 2 || ATOMIC_POINTER_LOCK_FREE != 2
#error expected lock-free types
#endif

_Atomic int counter = ATOMIC_VAR_INIT(10);
extern _Atomic(int) counter;
static const volatile atomic_int read_only = 6;
static atomic_schar narrow = -100;
static atomic_ushort halves[2] = {0xffff, 0x1234};
static atomic_ulong wide = 0;
static _Atomic(int *) pointer;
static int object;
static atomic_flag flag = ATOMIC_FLAG_INIT;

_Static_assert(sizeof(_Atomic(int)) == sizeof(int), "layout");
_Static_assert(_Alignof(atomic_ulong) == _Alignof(unsigned long), "alignment");
_Static_assert(_Generic(&counter, atomic_int *: 1, int *: 0), "atomic pointee type");
_Static_assert(_Generic(counter, atomic_int: 0, int: 1), "lvalue conversion");

int main(void) {
    atomic_bool truth;
    atomic_init(&truth, 8);
    if (!atomic_exchange(&truth, 0) || truth || atomic_load(&read_only) != 6)
        return 1;
    int i = 0;
    if (!atomic_is_lock_free(&halves[i++]) || i != 1)
        return 2;
    if ((halves[--i] += 2) != 1 || i || halves[0] != 1 || halves[1] != 0x1234)
        return 3;
    if (narrow++ != -100 || ++narrow != -98 || --narrow != -99 || narrow != -99)
        return 4;
    if (atomic_fetch_sub(&narrow, 30) != -99 || narrow != 127)
        return 5;
    if (atomic_fetch_or(&wide, 1UL << 63) != 0 || wide != (1UL << 63))
        return 6;
    if (atomic_fetch_xor(&wide, 3UL) != (1UL << 63) ||
        atomic_fetch_and(&wide, 1UL) != ((1UL << 63) | 3) || wide != 1UL)
        return 7;
    int expected = -1;
    if (atomic_compare_exchange_strong(&counter, &expected, 42) || expected != 10)
        return 8;
    if (!atomic_compare_exchange_weak_explicit(&counter, &expected, 42,
                                               memory_order_acq_rel, memory_order_acquire))
        return 9;
    if (counter != 42 || expected != 10)
        return 10;
    atomic_store_explicit(&counter, -7, memory_order_release);
    if (atomic_load_explicit(&counter, memory_order_consume) != -7 ||
        atomic_fetch_add(&counter, 11) != -7 || counter != 4)
        return 11;
    if (atomic_load(&pointer) != 0 || (pointer = &object) != &object ||
        atomic_exchange(&pointer, 0) != &object || pointer)
        return 12;
    int *expected_ptr = &object;
    if (atomic_compare_exchange_strong(&pointer, &expected_ptr, &object) || expected_ptr)
        return 13;
    if (atomic_flag_test_and_set(&flag) || !atomic_flag_test_and_set(&flag))
        return 14;
    atomic_flag_clear_explicit(&flag, memory_order_release);
    if (atomic_flag_test_and_set_explicit(&flag, memory_order_acquire))
        return 15;
    access_sequence(&counter);
    return counter != 8;
}
"""

NEGATIVES = {
    "_Atomic(const int) bad;": "requires an unqualified",
    "_Atomic(int[3]) bad;": "non-array",
    "_Atomic(float) bad;": "supports integer and object-pointer",
    "struct s { atomic_int bad; };": "atomic structure or union members",
    "void f(atomic_int a);": "atomic parameters by value",
    "atomic_int a; int *p = &a;": "supported symbol address",
    "void f(atomic_int *p) { *p *= 2; }": "atomic updates support only integer",
    "void f(int *p) { atomic_load(p); }": "pointer to an atomic object",
    "void f(const atomic_int *p) { atomic_store(p, 1); }": "non-const atomic object",
    "void f(atomic_int *p) { atomic_load_explicit(p, memory_order_release); }": "atomic load cannot",
    "void f(atomic_int *p) { atomic_store_explicit(p, 1, memory_order_acquire); }": "atomic store or flag clear cannot",
    "void f(atomic_int *p, memory_order m) { atomic_load_explicit(p, m); }": "integer constant expression",
    "void f(atomic_int *p, int *e) { atomic_compare_exchange_strong_explicit(p, e, 1, "
    "memory_order_release, memory_order_acquire); }": "stronger than success",
    "void f(atomic_int *p) { atomic_compare_exchange_strong(p, p, 1); }": "corresponding non-atomic type",
    "void f(_Atomic(int *) *p) { atomic_fetch_add(p, 1); }": "non-boolean integer objects",
}

THREADED_CEPHYR = r"""
#include <stdatomic.h>
atomic_int count;
static atomic_int ready;
static int payload;

void increment_from_cephyr(void) {
    for (int i = 0; i < 40000; ++i)
        ++count;
}

void publish(void) {
    payload = 12345;
    atomic_store_explicit(&ready, 1, memory_order_release);
}

int receive(void) {
    while (!atomic_load_explicit(&ready, memory_order_acquire)) {}
    return payload;
}
"""

THREADED_HOST = r"""
#include <pthread.h>
#include <stdatomic.h>
extern atomic_int count;
void increment_from_cephyr(void);
void publish(void);
int receive(void);

static int received;
static void *receiver(void *arg) {
    received = receive();
    return arg;
}

static void *worker(void *arg) {
    increment_from_cephyr();
    for (int i = 0; i < 40000; ++i)
        atomic_fetch_add_explicit(&count, 1, memory_order_relaxed);
    return arg;
}

int main(void) {
    pthread_t reader, workers[4];
    if (pthread_create(&reader, 0, receiver, 0))
        return 1;
    for (int i = 0; i < 4; ++i)
        if (pthread_create(&workers[i], 0, worker, 0))
            return 2;
    publish();
    if (pthread_join(reader, 0) || received != 12345)
        return 3;
    for (int i = 0; i < 4; ++i)
        if (pthread_join(workers[i], 0))
            return 4;
    return atomic_load(&count) != 320000;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    parser.add_argument("--cc", help="also exercise interoperability with a native pthread caller")
    args = parser.parse_args()
    host = platform.machine().lower()

    with tempfile.TemporaryDirectory(prefix="cephyr-atomics-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))

        def run(triple, *options, **settings):
            return subprocess.run(
                [args.cephyr.resolve(), "-std=c11", f"--target={triple}", *options],
                cwd=work, env=env, text=True, **settings
            )

        def rejected(triple, text, message):
            bad.write_text(text.rstrip() + "\n")
            result = run(triple, "-fsyntax-only", bad, capture_output=True)
            assert result.returncode > 0 and message in result.stderr, (text, result.stderr)

        sequence, source, bad = [work / f"{name}.c" for name in ("sequence", "atomics", "bad")]
        sequence.write_text(SEQUENCE)
        source.write_text(SEQUENCE + PROGRAM)
        qml = ("--qml-pass=canonicalize", "--qml-pass=strength_reduce")
        for triple, hosts, emulator in (
            ("x86_64-linux-gnu", ("x86_64", "amd64"), None),
            ("aarch64-linux-gnu", ("aarch64", "arm64"), args.qemu_aarch64),
        ):
            asm = work / f"{triple}.s"
            for passes in ((), qml):
                run(triple, *passes, "-S", sequence, "-o", asm, check=True)
                actual = CALLS.findall(asm.read_text())
                assert actual == EXPECTED, (triple, passes, actual)
            ast = work / "sequence.c11ast"
            run(triple, "--emit-ast=json", sequence, "-o", ast, check=True)
            run(triple, "-S", "-x", "c11-ast-json", ast, "-o", asm, check=True)
            actual = CALLS.findall(asm.read_text())
            assert actual == EXPECTED, (triple, "serialized AST", actual)
            executable = work / triple
            run(triple, *qml, source, "-o", executable, check=True)
            if host in hosts or emulator:
                command = [executable] if host in hosts else [emulator, executable]
                subprocess.run(command, cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked; execution skipped (no emulator)")
            for text, message in NEGATIVES.items():
                rejected(triple, "#include <stdatomic.h>\n" + text, message)
            rejected(triple, "#define __STDC_NO_ATOMICS__ 1\n", "special macro")

        rv64 = "riscv64-linux-gnu"
        bad.write_text("#if __STDC_NO_ATOMICS__ != 1\n#error atomics incorrectly advertised\n#endif\n")
        run(rv64, "-E", bad, check=True, capture_output=True)
        rejected(rv64, "_Atomic int value;", "require the bundled Linux AMD64 or AArch64")
        rejected(rv64, "#include <stdatomic.h>\n", "requires Cephyr's bundled Linux AMD64 or AArch64")

        if args.cc and host in ("x86_64", "amd64", "aarch64", "arm64"):
            amd64 = host in ("x86_64", "amd64")
            triple, arch = ("x86_64-linux-gnu", "AMD64") if amd64 else ("aarch64-linux-gnu", "AArch64")
            compiled = work / "threaded.o"
            runtime = args.cephyr.resolve().parent / "native-packages" / arch / "libc.a"
            peer, executable = work / "peer.c", work / "threaded"
            source.write_text(THREADED_CEPHYR)
            peer.write_text(THREADED_HOST)
            run(triple, *qml, "-c", source, "-o", compiled, check=True)
            subprocess.run([args.cc, "-std=c11", "-O2", "-pthread", peer, compiled, runtime, "-o", executable],
                           cwd=work, env=env, check=True)
            subprocess.run([executable], cwd=work, env=env, check=True, timeout=30)


if __name__ == "__main__":
    main()
