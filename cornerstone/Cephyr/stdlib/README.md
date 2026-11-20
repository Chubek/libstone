# Cephyr libc

This directory is Cephyr's freestanding C11 library for static AMD64, AArch64,
and RV64IM Linux LP64 programs. `build.sh` uses an independently configured trusted target compiler;
the library never falls back to host headers or host libc.

```sh
CC=/path/to/clang AR=/path/to/llvm-ar \
  CEPHYR_LIBC_TARGET=riscv64-linux-gnu \
  CEPHYR_LIBC_TARGET_FLAGS='-march=rv64im -mabi=lp64' \
  ./build.sh /path/to/output
```

Without a target override, `build.sh` builds for `x86_64-linux-gnu`.
The output is `libc.a` plus a separate `crt0.o`. All C sources are compiled
with `-ffreestanding -fno-builtin -nostdinc`. For a dedicated cross compiler
that does not accept Clang's `--target=...` flag, set
`CEPHYR_LIBC_TARGET_OPTION=''`.

## Implemented surface

- C11 scalar/type headers for the LP64 data model, with target-specific plain `char`.
- ASCII `ctype` functions.
- The C memory/string functions, plus `strnlen`; collation uses the C locale.
- Integer text conversion with base detection and `ERANGE`, integer absolute
  value/division, `qsort`, `bsearch`, and deterministic `rand`/`srand`.
- Descriptor-backed `stdin`, `stdout`, and `stderr`; unbuffered character,
  line, and block I/O.
- `printf` output for `%d`, `%i`, `%u`, `%o`, `%x`, `%X`, `%c`, `%s`, `%p`,
  `%n`, and `%%`, including ordinary flags, widths, precisions, and integer
  length modifiers. `snprintf` always bounds and terminates output when its
  size is nonzero.

This is intentionally not a complete hosted libc. There is no locale, floating
conversion, scanf family, filesystem `FILE` creation, threading, or wide/multi-
byte conversion yet. An unsupported formatted-output conversion returns `-1`
and sets `errno` to `EINVAL`; it is never silently formatted incorrectly.
Results too large for the `int` return type fail with `EOVERFLOW`. Streams are
currently unbuffered, so `fflush` is a successful no-op.

## Variadic ABI boundary

The library sources are compiled by the external target compiler, whose target
variadic ABI is trusted. Cephyr-generated calls are a separate capability.
When the driver defines `__CEPHYR_HAS_VARIADIC_CALLS`, Cephyr's `stdio.h`
declares `printf`, `fprintf`, `sprintf`, and `snprintf` for direct calls with
supported integer and pointer arguments. Without that macro it defines
`__CEPHYR_VARIADIC_IO_UNAVAILABLE` instead. Cephyr does not support variadic
function definitions or `va_list`, even when it can make direct calls:
`stdio.h` does not declare the `v` forms under `__CEPHYR__`, and including
`stdarg.h` in that profile is an error.

The current vendor parser also cannot faithfully represent `long double`,
explicit alignment declarations, or aggregate return values. Under
`__CEPHYR__`, the headers therefore expose
`__CEPHYR_MAX_ALIGN_T_UNAVAILABLE`, `__CEPHYR_ALIGNAS_UNAVAILABLE`, and
`__CEPHYR_STRUCT_VALUE_API_UNAVAILABLE` rather than substituting ABI-wrong
types or declarations. External target compilation receives the standard
declarations for these interfaces. Cephyr's `assert` omits the function name
because its frontend does not represent `__func__`; it still reports the
expression, file and line and calls `abort`.

`tests/smoke.c` is intended for cross compilation and static linking with this
runtime. It covers strings, ctype, conversion, sorting, bounded formatting, and
the mmap allocator. `tests/io.c` covers raw standard input/output, and
`tests/headers-cephyr.c` checks the Cephyr header profile.
`tests/variadic-cephyr.c` exercises a direct scalar variadic call spanning
argument registers and the stack. Run target binaries on matching Linux hardware
or under an emulator.
