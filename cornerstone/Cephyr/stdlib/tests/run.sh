#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?usage: sh tests/run.sh BUILD-DIRECTORY}
CC=${CC:-clang}
AR=${AR:-llvm-ar}
LD_LLD=${LD_LLD:-ld.lld}
export CC AR

CEPHYR_STDLIB_TEST='value=1'
CEPHYR_STDLIB_TEST_EMPTY=''
export CEPHYR_STDLIB_TEST CEPHYR_STDLIB_TEST_EMPTY
unset CEPHYR_STDLIB_TEST_ABSENT

host=$(uname -m)
for arch in x86_64 aarch64 riscv64; do
    target=$arch-linux-gnu
    output=$build/$target
    case "$arch" in
        x86_64) set -- ;;
        aarch64) set -- -march=armv8-a ;;
        riscv64) set -- -march=rv64im -mabi=lp64 ;;
    esac
    CEPHYR_LIBC_TARGET="$target" CEPHYR_LIBC_TARGET_FLAGS="$*" \
        "$root/build.sh" "$output"

    for source in smoke io files general time multibyte nonlocal scanf; do
        "$CC" --target="$target" "$@" -std=c11 -O2 -Wall -Wextra -Werror \
            -ffreestanding -fno-builtin -fno-stack-protector -nostdinc \
            -I "$root/include" -c "$root/tests/$source.c" -o "$output/test-$source.o"
        extra=
        if [ "$source" = nonlocal ]; then
            extra="$output/nonlocal-registers.o"
            "$CC" --target="$target" "$@" -c "$root/tests/nonlocal-registers.S" -o "$extra"
        fi
        "$LD_LLD" -static --fatal-warnings -o "$output/test-$source" \
            "$output/crt0.o" "$output/test-$source.o" ${extra:+"$extra"} "$output/libc.a"
    done
    # A custom CRT need not define environ. Link but do not run with no startup.
    "$LD_LLD" -static --fatal-warnings -e main -o "$output/test-no-crt" \
        "$output/test-general.o" "$output/libc.a"

    run() {
        if [ "$host" = "$arch" ]; then
            "$@"
        else
            "$emulator" "$@"
        fi
    }
    case "$arch" in
        x86_64) emulator=${CEPHYR_TEST_QEMU_AMD64:-qemu-x86_64} ;;
        aarch64) emulator=${CEPHYR_TEST_QEMU_AARCH64:-qemu-aarch64} ;;
        riscv64) emulator=${CEPHYR_TEST_QEMU:-qemu-riscv64} ;;
    esac
    if [ "$host" != "$arch" ] && ! command -v "$emulator" >/dev/null 2>&1; then
        echo "$target: statically linked; execution skipped ($emulator not installed)"
        continue
    fi

    run "$output/test-smoke"
    run "$output/test-time"
    run "$output/test-multibyte"
    run "$output/test-nonlocal"
    printf '18? 24\n' | run "$output/test-scanf"
    text=$(printf 'c!' | run "$output/test-io" pipe)
    test "$text" = C
    mkdir -p "$output/remove-dir"
    run "$output/test-files" "$output/remove-dir"
    text=$(run "$output/test-general")
    test "$text" = BN
    status=0
    text=$(run "$output/test-general" quick) || status=$?
    test "$status" = 17
    test "$text" = 21
    status=0
    text=$(run "$output/test-general" immediate) || status=$?
    test "$status" = 23
    test -z "$text"
    echo "$target: runtime tests passed"
done
