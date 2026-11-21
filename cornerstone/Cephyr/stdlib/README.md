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
  code-generation guarantee. `FLT_ROUNDS` reflects the hardware rounding mode
  on AMD64 and AArch64; on RV64IM it is indeterminable.
- C11 §7.6 `<fenv.h>` on Linux AMD64 and little-endian AArch64. This does
  not enable `#pragma STDC FENV_ACCESS` or Annex F in the frontend.
- ASCII `ctype` functions and the §7.30 wide classification and mapping
  functions (`<wctype.h>`) in the C locale.
- The C memory/string functions, plus `strnlen`; collation uses the C locale.
- All §7.29.4.2–6 wide string and memory operations, including independent
  `wcstok` cursors and C-locale collation. Wide memory operations preserve
  any `wchar_t` value, whether or not it encodes as external text.
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
- §7.14 `<signal.h>` with `signal`, `raise`, the six ISO C signals, and
  `sig_atomic_t` with matching §7.20.3 limits in `<stdint.h>`. Dispositions
  apply to both `raise` and Linux delivery from outside the program. Handler
  registrations persist; Linux blocks the delivered signal in its handler.
  No POSIX signal APIs are exposed. See [signal details](RUNTIME.md#signals).
- §7.22 general utilities: integer text conversion with base detection and
  `ERANGE`, integer absolute value/division, `qsort`, `bsearch`, deterministic
  `rand`/`srand`, `getenv`, `aligned_alloc` and the other allocation functions,
  and separate `atexit`/`at_quick_exit` handler lists. The shared integer
   parser also handles `wcstol`, `wcstoll`, `wcstoul`, `wcstoull`, `wcstoimax`,
   and `wcstoumax` (§7.29.4.1.2 and §7.8.2.4).
- Floating text conversion (§§7.22.1.1, 7.22.1.3, 7.29.4.1.1):
  `atof`, `strtof`, `strtod`, `wcstof`, and `wcstod` accept decimal and
  hexadecimal forms, infinity, and quiet NaN in the C locale. Each conversion
  rounds directly to its destination format. External callers have all five
  functions on the three runtime targets; Cephyr declarations require native
  scalar floating-point calls. See [range and rounding details](RUNTIME.md#floating-text-conversion).
- §7.21 narrow streams: standard streams, `fopen`, `freopen`, `fclose`, file
  positioning, one byte of `ungetc`, and character, line, and block I/O.
  The Linux text and binary modes both transfer bytes without translation.
  `stderr` starts unbuffered; other streams are line buffered on terminals
  and fully buffered elsewhere. `setvbuf`/`setbuf` configure buffering before
  I/O, and `fflush(NULL)` flushes pending output on all streams. Normal
  `exit` flushes output after running handlers; `quick_exit` and `_Exit` do not.
- All §7.29.3 unformatted wide I/O, including `fwide`, character and line I/O,
  and one wide character of `ungetwc`. Streams are initially unoriented,
  including `stdin`, `stdout`, and `stderr`. The first applicable I/O call or
  explicit `fwide` request fixes orientation until a successful `freopen`.
  Wide streams use the ASCII `C` locale, including embedded null characters
  in files; unrepresentable input or output reports `EILSEQ`.
- §7.21 file operations: `remove`, `rename`, `tmpfile`, and `tmpnam`.
  `tmpfile` uses anonymous or immediately unlinked files under `/tmp`; prefer
  it over `tmpnam`, which generates a name but does not reserve it. If using
  such a name to create a file, use the exclusive `fopen` mode (`"wx"`).
  Mode-only `freopen(NULL, ...)` is not supported.
- `printf` output for `%d`, `%i`, `%u`, `%o`, `%x`, `%X`, `%c`, `%s`, `%lc`,
  `%ls`, `%p`, `%n`, and `%%`, including ordinary flags, widths, precisions,
  and integer length modifiers. `%lc` and `%ls` encode ASCII wide characters
  in the C locale; unrepresentable values give `-1` with `EILSEQ`. For
  `%ls`, the precision bounds the bytes converted, and `snprintf` still
  counts the complete result even if its output buffer is too small.
- §7.21.6 formatted input (`scanf`, `fscanf`, `sscanf`, and their `v` forms)
  for `%d`, `%i`, `%u`, `%o`, `%x`, `%X`, `%c`, `%s`, `%[`, `%p`, `%n`,
  and `%%`. Includes field widths, assignment suppression, integer length
  modifiers, and ASCII-only `%lc`, `%ls`, and `%l[`. Stream matching keeps
  the first nonmatching byte unread using one byte of pushback, even on pipes.
  `%p` accepts `0x` (or `0X`) followed by hexadecimal digits, matching the
  bundled `%p` output; use it to read pointers printed within the same run.
  Scansets support ascending byte ranges, parsed left to right; a hyphen
  that cannot form such a range is literal.
- §7.29.2 wide formatted input (`wscanf`, `fwscanf`, `swscanf`, and their
  `v` forms) supports the same nonfloating conversion specifiers. Field widths and
  `%n` count wide characters. `%c`, `%s`, and `%[` without `l` encode
  their input with `wcrtomb` in the ASCII C locale; with `l` they copy wide
  characters directly. Scansets interpret ascending wide-character ranges
  the same way as the byte scanner. Stream calls establish wide orientation
  and leave the first nonmatching wide character unread, even on pipes.

An unsupported narrow formatted-output conversion returns `-1` with
`EINVAL`; a result larger than `INT_MAX` fails with `EOVERFLOW`.

Narrow formatted input supports `aAeEfFgG` for `float *` and, with `l`,
`double *`. Wide floating input and narrow `long double` input stop with
`ENOSYS`. Malformed directives stop with `EINVAL`; a width larger than
`SIZE_MAX` stops with `EOVERFLOW`. Integer values outside the destination
range stop with `ERANGE` rather than writing the result (C11 does not define
out-of-range assignments). Wide stream decoding and text fields that convert
between byte and wide characters report `EILSEQ` for unrepresentable input.
These stops return the number of assignments
already made, or `EOF` if none (`EILSEQ` follows the ordinary input-failure
rule: `EOF` only before the first completed conversion). The scanner stops
before consuming input for a malformed or unsupported directive. Fixed-width
character fields and wide fields stage input so that a failed field does not
write its destination; staging large fields can fail with `ENOMEM`.

## Remaining C11 conformance blockers

This inventory is not a conformance claim. In particular:

| Clauses | Known blockers |
| --- | --- |
| §4¶6, §§5.2.4.2.2, 7.7, 7.16 | The bundled tree contains all nine freestanding headers. Native `<stdarg.h>` supports scalar arguments on SysV AMD64 and little-endian AAPCS64; RV64IM still needs a `va_list` ABI. AMD64 supports `long double` scalar arithmetic, but not by-value calls. RV64IM binary32/binary64 arithmetic and conversions use compiler-runtime helpers. `<float.h>` ABI format limits do not imply native compiler support. |
| §5.1.2.2, §7.22.4 | The Linux CRT supports ordinary hosted `main` and normal termination; it is not a general platform CRT. `system` is missing. See [startup details](RUNTIME.md). |
| §§7.3, 7.6, 7.12, 7.25 | No complex arithmetic library, math library, or type-generic math. RV64IM has no `<fenv.h>` implementation. |
| §§7.8, 7.22 | APIs returning `imaxdiv_t`, `div_t`, `ldiv_t`, and `lldiv_t` by value are also exposed to native Cephyr callers on bundled RV64IM LP64. Other Cephyr targets lack the required aggregate ABI. `strtold` is not implemented or declared. |
| §7.13 | The runtime contains `setjmp` and `longjmp`, but Cephyr still lacks the returns-twice call semantics required to expose these to native callers. |
| §§7.17, 7.26 | Atomics and threads are outside the advertised C11 conditional feature set. |
| §§7.21.6, 7.29.2, 7.29.4.1.1 | Narrow formatted I/O lacks `long double` conversions; wide formatted I/O lacks floating conversions. `wcstold` is missing. Native access to the `v` interfaces requires target `va_list` support and declarations. |

## Variadic ABI boundary

The library sources are compiled by the external target compiler, whose target
variadic ABI is trusted. Cephyr-generated calls are a separate capability.
When the driver defines `__CEPHYR_HAS_VARIADIC_CALLS`, Cephyr's `stdio.h`
declares the non-`v` formatted input and output functions for direct calls with
supported integer and pointer arguments. Without that macro it defines
`__CEPHYR_VARIADIC_IO_UNAVAILABLE` instead. The ability to make direct
variadic calls does not itself imply support for variadic definitions,
`va_list`, or the `v` forms.

Cephyr supports fundamental alignment, `max_align_t`, and `offsetof`. The
bundled RV64IM LP64 target exposes `div`, `ldiv`, `lldiv`, and `imaxdiv` to
native callers. On targets without the required ABI, the library headers
define `__CEPHYR_STRUCT_VALUE_API_UNAVAILABLE` instead of substituting
ABI-incompatible declarations. External target compilation receives the
standard declarations.
`assert` reports the expression, file, line, and enclosing function before
calling `abort`.

The Cephyr profile defines `__CEPHYR_DIFFTIME_UNAVAILABLE` on targets without
native scalar floating-point calls. It defines `__CEPHYR_NONLOCAL_JUMPS_UNAVAILABLE`
on all targets because ordinary native MIR calls do not model returns-twice
semantics. External compilation sees both `difftime` and the returns-twice
`setjmp` declaration.

Run `sh tests/run.sh /path/to/build-dir` with Clang, `llvm-ar`, and `ld.lld`
to statically link and test the packaged library without host headers or libc.
It checks the stream lifecycle, buffering, file operations, pipe behavior,
locale, allocation, startup environment, termination, calendar and clock
operations, restartable conversions, floating text conversion, formatted input,
unformatted wide streams, wide strings, signal delivery, and nonlocal jumps
(including register restoration).
It runs on matching Linux hardware or with a
user-mode QEMU on `PATH`. Emulator paths can also be provided via
`CEPHYR_TEST_QEMU_AMD64`, `CEPHYR_TEST_QEMU_AARCH64`, and `CEPHYR_TEST_QEMU`
(RV64). If a target cannot run, the suite still builds and statically links it.

`tests/headers-cephyr.c` checks the Cephyr header profile.
`tests/variadic-cephyr.c` exercises direct `snprintf` and `sscanf` calls spanning
argument registers and the stack, plus `%lc` and `%ls` buffer and stream output.
`tests/time-multibyte-cephyr.c` exercises
the scalar and pointer APIs, including `difftime` where the native ABI supports
it, wide stream positioning and round trips, wide string comparison, and wide
integer conversion. The wide portions construct `wchar_t` arrays using
ordinary execution-set codes, so they do not depend on frontend support for
wide literals. Run target binaries on matching Linux hardware or under an emulator.

`python3 ../tests/test_cephyr_c11_signals.py --cephyr /path/to/cephyr` builds
Cephyr-generated signal callers and checks delivery, disposition replacement,
external delivery, and `abort`. Pass `--qemu-aarch64` and `--qemu` for the cross
targets, or `--external-dir /path/to/build-dir` to test binaries built by
`tests/run.sh` as well.

`python3 ../tests/test_cephyr_c11_strtofp.py --cephyr /path/to/cephyr` checks
floating text conversion from native AMD64 and AArch64 callers and checks
declaration gating for RV64IM. Pass `--qemu-aarch64` and `--qemu` for the cross
targets. The external-compiler suite tests the actual conversions on RV64IM.
