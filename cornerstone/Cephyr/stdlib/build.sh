#!/bin/sh
set -eu

# The libc is deliberately built by a trusted external target compiler, not by
# Cephyr's still-evolving C frontend.  Override these variables with absolute
# tool paths/flags when packaging a toolchain.
TARGET=${CEPHYR_LIBC_TARGET:-x86_64-linux-gnu}
CC=${CC:-${TARGET}-gcc}
AR=${AR:-ar}
TARGET_OPTION=${CEPHYR_LIBC_TARGET_OPTION-}
case $TARGET in
    riscv64-*-linux-gnu|riscv64-linux-gnu)
        default_flags='-march=rv64im -mabi=lp64 -mno-relax'
        suffix=
        ;;
    x86_64-*-linux-gnu|x86_64-linux-gnu)
        default_flags=
        suffix=-amd64
        ;;
    aarch64-*-linux-gnu|aarch64-linux-gnu)
        default_flags='-march=armv8-a'
        suffix=-aarch64
        ;;
    *) echo "unsupported Cephyr libc target: $TARGET" >&2; exit 1 ;;
esac
TARGET_FLAGS=${CEPHYR_LIBC_TARGET_FLAGS-$default_flags}
EXTRA_CFLAGS=${CEPHYR_LIBC_CFLAGS:-}

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=${1:-"$root/build"}
mkdir -p "$build"

compile() {
    # Tool flags are trusted packaging inputs with whitespace-separated argv.
    (
        set -f
        "$CC" $TARGET_OPTION $TARGET_FLAGS $EXTRA_CFLAGS \
            -Wall -Wextra -Werror -ffreestanding -fno-builtin \
            -fno-stack-protector -nostdinc -I "$root/include" "$@"
    )
}

sources='string ctype stdlib strtofp stdio scanf stream assert allocator runtime time multibyte wideprintf fenv'
set --
for source in $sources; do
    object="$build/$source.o"
    compile -std=c11 -O2 \
        -c "$root/src/$source.c" -o "$object"
    set -- "$@" "$object"
done

if [ "$suffix" != -amd64 ]; then
    for source in real_bits real_abi; do
        object="$build/$source.o"
        compile -std=c11 -O2 -fno-lto -fno-fast-math -fvisibility=hidden \
            -c "$root/src/$source.c" -o "$object"
        set -- "$@" "$object"
    done
fi

# Baseline Armv8-A uses in-line exclusive instructions, not libgcc's
# optional LSE dispatch routines. RV64IM has no atomic extension.
if [ -n "$suffix" ]; then
    atomic_flags=
    if [ "$suffix" = -aarch64 ]; then atomic_flags=-mno-outline-atomics; fi
    compile -std=c11 -O2 -fno-lto -fvisibility=hidden $atomic_flags \
        -c "$root/src/atomic.c" -o "$build/atomic.o"
    set -- "$@" "$build/atomic.o"
fi

compile -c "$root/src/syscall$suffix.S" -o "$build/syscall.o"
compile -c "$root/src/setjmp$suffix.S" -o "$build/setjmp.o"
compile -c "$root/src/start$suffix.S" -o "$build/crt0.o"

# Start a fresh archive: incremental packaging must not retain removed
# members (in particular the superseded soft-float runtime).
rm -f "$build/libc.new.a"
"$AR" rcs "$build/libc.new.a" "$@" "$build/syscall.o" "$build/setjmp.o"
mv "$build/libc.new.a" "$build/libc.a"

echo "$build/libc.a"
echo "$build/crt0.o"
