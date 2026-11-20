#!/bin/sh
set -eu

# The libc is deliberately built by a trusted external target compiler, not by
# Cephyr's still-evolving C frontend.  Override these variables with absolute
# tool paths/flags when packaging a toolchain.
CC=${CC:-clang}
AR=${AR:-llvm-ar}
TARGET=${CEPHYR_LIBC_TARGET:-x86_64-linux-gnu}
TARGET_OPTION=${CEPHYR_LIBC_TARGET_OPTION---target=$TARGET}
case $TARGET in
    riscv64-*-linux-gnu|riscv64-linux-gnu)
        default_flags='-march=rv64im -mabi=lp64'
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

set --
for source in string ctype stdlib stdio assert allocator runtime; do
    object="$build/$source.o"
    compile -std=c11 -O2 \
        -c "$root/src/$source.c" -o "$object"
    set -- "$@" "$object"
done

compile -c "$root/src/syscall$suffix.S" -o "$build/syscall.o"
compile -c "$root/src/start$suffix.S" -o "$build/crt0.o"

"$AR" rcs "$build/libc.a" "$@" "$build/syscall.o"

echo "$build/libc.a"
echo "$build/crt0.o"
