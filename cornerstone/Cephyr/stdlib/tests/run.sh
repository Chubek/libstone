#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:?usage: sh tests/run.sh BUILD-DIRECTORY}
AR=${AR:-ar}
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
        x86_64) CC=${CEPHYR_TEST_CC_AMD64:-$target-gcc}; set -- ;;
        aarch64) CC=${CEPHYR_TEST_CC_AARCH64:-$target-gcc}; set -- -march=armv8-a ;;
        riscv64) CC=${CEPHYR_TEST_CC_RV64:-$target-gcc}; set -- -march=rv64im -mabi=lp64 -mno-relax ;;
    esac
    CEPHYR_LIBC_TARGET="$target" CEPHYR_LIBC_TARGET_FLAGS="$*" \
        "$root/build.sh" "$output"

    for source in smoke io files general time multibyte nonlocal scanf wide-scanf wide-stream wide-string wide-printf-cephyr variadic-cephyr signals-cephyr system-failures strtofp-cephyr strtold printf-float-cephyr fenv-cephyr; do
        "$CC" "$@" -std=c11 -O2 -Wall -Wextra -Werror \
            -ffreestanding -fno-builtin -fno-stack-protector -nostdinc \
            -I "$root/include" -c "$root/tests/$source.c" -o "$output/test-$source.o"
        extra=
        wrap=
        if [ "$source" = nonlocal ]; then
            extra="$output/nonlocal-registers.o"
            "$CC" "$@" -c "$root/tests/nonlocal-registers.S" -o "$extra"
        elif [ "$source" = system-failures ]; then
            wrap='-Wl,--wrap=__cephyr_clone,--wrap=__cephyr_execve,--wrap=__cephyr_wait4'
        fi
        "$CC" "$@" -nostdlib -no-pie -static $wrap -o "$output/test-$source" \
            "$output/crt0.o" "$output/test-$source.o" ${extra:+"$extra"} "$output/libc.a"
    done
    # A custom CRT need not define environ. Link but do not run with no startup.
    "$CC" "$@" -nostdlib -no-pie -static -Wl,-e,main -o "$output/test-no-crt" \
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
    run "$output/test-wide-string"
    run "$output/test-signals-cephyr"
    run "$output/test-system-failures"
    run "$output/test-strtofp-cephyr"
    run "$output/test-strtold"
    run "$output/test-printf-float-cephyr"
    run "$output/test-fenv-cephyr"
    if [ "$arch" != riscv64 ]; then
        run "$output/test-fenv-cephyr" xr
        run "$output/test-fenv-cephyr" xu
        if [ "$arch" = x86_64 ]; then
            run "$output/test-fenv-cephyr" sr
            run "$output/test-fenv-cephyr" su
        fi
    fi
    text=$(run "$output/test-wide-printf-cephyr")
    test "$text" = "wide:-7!"
    text=$(run "$output/test-variadic-cephyr")
    test "$text" = "-7 65535 -9000000000 4 5 6 7 stack 12345678901234"
    text=$(printf 'Z!' | run "$output/test-wide-stream" 2>"$output/test-wide-stream.stderr")
    test "$text" = Q
    test "$(cat "$output/test-wide-stream.stderr")" = "wide stream test: Invalid argument
Invalid argument"
    run "$output/test-nonlocal"
    printf '18? 24\n' | run "$output/test-scanf"
    printf '18? 24\n' | run "$output/test-wide-scanf"
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
