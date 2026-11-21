# Cephyr libc

This directory provides a subset of the C11 library for static AMD64, AArch64,
and RV64IM Linux LP64 programs. `build.sh` uses an independently configured
trusted target compiler; the library never falls back to host headers or host libc.

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

- C11 scalar/type headers for the LP64 data model, with target-specific plain `char`,
  floating-point format limits in `<float.h>`, and the alternative spellings
  from `<iso646.h>`. A format reported by `<float.h>` is not a frontend
  code-generation guarantee. The library reports `FLT_ROUNDS` as indeterminable.
- ASCII `ctype` functions and the §7.30 wide classification and mapping
  functions (`<wctype.h>`) in the C locale.
- The C memory/string functions, plus `strnlen`; collation uses the C locale.
- C11 §7.11 localization: `setlocale` and `localeconv` support the ASCII `C`
  locale only. `POSIX` is an alias, and the native locale selected by `""` is
  always `C`. The §7.22.7–8 multibyte/wide conversions use this stateless,
  single-byte encoding and reject non-ASCII input with `EILSEQ`. The four
  §7.28 `<uchar.h>` functions and the §7.29.6 restartable `<wchar.h>`
  conversions use the same encoding. A UTF-16 leading surrogate can be
  pending in `c16rtomb`, but no supplementary character is representable.
- All of §7.27 `<time.h>` and §7.29.5 `wcsftime`. `time` and `timespec_get`
  use the Linux realtime clock; `clock` uses process CPU time in microseconds.
  Calendar time is signed 64-bit Unix time, with Gregorian conversion,
  local time fixed to UTC, and no DST, `TZ` database, or leap-second tracking.
- §7.13 `<setjmp.h>` for external target C compilers, including AMD64 SysV,
  AArch64 AAPCS64, and soft-float RV64 LP64. This is plain ISO `setjmp`:
  no signal-mask restoration, `sigsetjmp`, or shadow-stack integration.
- §7.22 general utilities: integer text conversion with base detection and
  `ERANGE`, integer absolute value/division, `qsort`, `bsearch`, deterministic
  `rand`/`srand`, `getenv`, `aligned_alloc` and the other allocation functions,
  and separate `atexit`/`at_quick_exit` handler lists.
- §7.21 narrow streams: standard streams, `fopen`, `freopen`, `fclose`, file
  positioning, one byte of `ungetc`, and character, line, and block I/O.
  The Linux text and binary modes both transfer bytes without translation.
  `stderr` starts unbuffered; other streams are line buffered on terminals
  and fully buffered elsewhere. `setvbuf`/`setbuf` configure buffering before
  I/O, and `fflush(NULL)` flushes pending output on all streams. Normal
  `exit` flushes output after running handlers; `quick_exit` and `_Exit` do not.
- §7.21 file operations: `remove`, `rename`, `tmpfile`, and `tmpnam`.
  `tmpfile` uses anonymous or immediately unlinked files under `/tmp`; prefer
  it over `tmpnam`, which generates a name but does not reserve it. If using
  such a name to create a file, use the exclusive `fopen` mode (`"wx"`).
  Mode-only `freopen(NULL, ...)` is not supported.
- `printf` output for `%d`, `%i`, `%u`, `%o`, `%x`, `%X`, `%c`, `%s`, `%p`,
  `%n`, and `%%`, including ordinary flags, widths, precisions, and integer
  length modifiers. `snprintf` always bounds and terminates output when its
  size is nonzero.

This is not a complete hosted libc. Missing areas include `scanf`, floating
conversion and math, `%lc`/`%ls`, the remaining `<wchar.h>` APIs (wide
streams, formatted wide I/O, wide numeric conversion, and wide string
manipulation), locales beyond `C`, `system`, signal handling, atomics, and
threading. An unsupported formatted-output conversion returns
`-1` with `EINVAL`; a result larger than `INT_MAX` fails with `EOVERFLOW`.

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
declarations for these interfaces. `assert` reports the expression, file,
line, and enclosing function before calling `abort`.

The Cephyr profile defines `__CEPHYR_DIFFTIME_UNAVAILABLE` on targets without
native scalar floating-point calls. It defines `__CEPHYR_NONLOCAL_JUMPS_UNAVAILABLE`
on all targets because ordinary native MIR calls do not model returns-twice
semantics. External compilation sees both `difftime` and the returns-twice
`setjmp` declaration.

Run `sh tests/run.sh /path/to/build-dir` with Clang, `llvm-ar`, and `ld.lld`
to statically link and test the packaged library without host headers or libc.
It checks the stream lifecycle, buffering, file operations, pipe behavior,
locale, allocation, startup environment, termination, calendar and clock
operations, restartable conversions, and nonlocal jumps (including
register restoration). It runs on matching Linux hardware or with a
user-mode QEMU on `PATH`. Emulator paths can also be provided via
`CEPHYR_TEST_QEMU_AMD64`, `CEPHYR_TEST_QEMU_AARCH64`, and `CEPHYR_TEST_QEMU`
(RV64). If a target cannot run, the suite still builds and statically links it.

`tests/headers-cephyr.c` checks the Cephyr header profile.
`tests/variadic-cephyr.c` exercises a direct scalar variadic call spanning
argument registers and the stack. `tests/time-multibyte-cephyr.c` exercises
the scalar and pointer APIs, including `difftime` where the native ABI supports
it. It does not use returns-twice calls. Run target binaries on matching
Linux hardware or under an emulator.
