# Cephyr Linux runtime

This runtime targets statically linked Linux executables for AMD64 SysV LP64,
AArch64 AAPCS64 LP64, and RV64IM LP64. It has no host-libc fallback and is not
a dynamic ELF startup runtime.

`src/start-amd64.S`, `src/start-aarch64.S`, and `src/start.S` implement the CRT entry. They read `argc`,
`argv`, and `envp` from the Linux initial stack, initialize `environ`, maintain
16-byte stack alignment, call `main`, and pass its result to `exit`. The RV64
entry also initializes `gp`.

`src/syscall-amd64.S`, `src/syscall-aarch64.S`, and `src/syscall.S` provide direct
Linux wrappers for `read`, `write`, `close`,
`lseek`, `open`, `openat`, `mmap`, and `munmap`, plus process termination and
`abort`. Internal wrappers support terminal detection, file removal/rename,
temporary-name entropy, and LP64 `clock_gettime`. Kernel errors become `-1`
and update `errno`.

`src/runtime.c` provides `getenv`, `atexit`/`exit`, and
`at_quick_exit`/`quick_exit`. Each exit-handler list supports 32 registrations,
run in reverse order, and the two lists never mix. `exit` calls the weak stdio
flush hook after running the handlers. `quick_exit` and `_Exit` do not flush
stdio. Linux closes the process's descriptors at termination.

`system(command)` runs `/bin/sh -c -- command` with the calling process's
environment and waits for that shell. The result is a Linux wait status (e.g.
`37 << 8` for `exit 37`, or the signal number in the low seven bits for signal
termination). Failure to execute the shell looks like `exit 127`; inability
to create the child or obtain its status returns `-1` with `errno` set.
`system(NULL)` actually runs `:` to check whether the shell is available.
While waiting, the runtime ignores `SIGINT` and `SIGQUIT` and blocks `SIGCHLD`;
the shell inherits the original mask and any ignored dispositions.

`src/allocator.c` provides `malloc`, `aligned_alloc`, `calloc`, `realloc`, and
`free` using one anonymous mapping per allocation. `aligned_alloc` accepts
power-of-two alignments and requires the requested size to be a multiple of
the alignment. `free` accepts aligned allocations; `realloc` preserves their
contents, not the requested extended alignment. This is not an arena allocator.

`src/stream.c` implements buffered byte and wide streams on Linux descriptors.
Wide I/O shares descriptor buffering but uses the ASCII C-locale conversions
with per-stream `mbstate_t`. `fgetpos`/`fsetpos` save and restore this state
along with the logical offset; the C locale always remains in the initial
conversion state. `fpos_t` is now a structure, so recompile callers that were
built with the old byte-only `long` definition. Logical positions account for
read-ahead, pending output, and byte pushback. Wide pushback uses a separate
one-character slot and leaves the underlying file position unchanged.
Append writes use `O_APPEND`; `a+` starts reading from the beginning.
Temporary files use `O_TMPFILE` when available, or `O_CREAT|O_EXCL` followed by
unlink. Temporary names use Linux `getrandom`; the fallback is for filesystems
without `O_TMPFILE`, not for kernels without `getrandom`.

`src/time.c` uses `CLOCK_REALTIME` for Unix time and `CLOCK_PROCESS_CPUTIME_ID`
for user plus system CPU time. Calendar conversion and formatting are local
to the runtime; neither timezone configuration nor locale data files are read.
On RV64IM, `difftime` builds the IEEE 754 binary64 result with integer
arithmetic rather than linking compiler support routines.

`src/setjmp-amd64.S`, `src/setjmp-aarch64.S`, and `src/setjmp.S` save and
restore each ABI's nonvolatile registers, stack pointer, and return address
(including AArch64's nonvolatile FP registers). The RV64 implementation
assumes the integer LP64 ABI, not LP64F/LP64D. The routines do not support
hardware or compiler shadow stacks, authenticated jump buffers, or POSIX
signal-mask management. Cephyr-generated code cannot call `setjmp` correctly
until native MIR can represent calls that return twice.

## Signals

`src/runtime.c` implements ISO C `signal` and `raise` using Linux
`rt_sigaction`, `rt_sigprocmask`, and `tgkill`. `sig_atomic_t` is a signed
32-bit `int`; use `volatile sig_atomic_t` for flags shared with a handler. The runtime
accepts `SIGABRT`, `SIGFPE`, `SIGILL`, `SIGINT`, `SIGSEGV`, and `SIGTERM`;
other numbers produce `EINVAL`. The six signals default to Linux termination,
with core dumps subject to Linux policy for `SIGABRT`, `SIGFPE`, `SIGILL`,
and `SIGSEGV`. Ignored dispositions may be inherited across Linux `exec`.

Handlers remain installed after delivery; the kernel blocks the delivered
signal while its handler runs and restarts eligible syscalls. `raise` sends to
the calling thread and temporarily unblocks the requested signal if it was
blocked, then restores that bit in the mask. On AMD64 the assembly restorer
issues `rt_sigreturn`; AArch64 and RV64 use the kernel-provided trampoline.
`abort` allows an installed `SIGABRT` handler to run, but still terminates if
the handler returns or the signal is ignored. It does not run exit handlers
or flush streams.

Handlers remain subject to C11's restrictions, including on library calls
from asynchronous handlers and returning from handlers for computational
exceptions. No POSIX `sigaction`, public signal-mask API, alternate signal
stack, or threaded signal semantics are provided.

## Floating environment

On AMD64 and AArch64, `<fenv.h>` reads and writes hardware state. RV64IM LP64
uses Libstone's integer-only arithmetic in `src/real_bits.c`, with the target
calling conventions implemented in `src/real_abi.c`. Operations round once,
to nearest with ties to even, and do not record floating exception flags.
Consequently RV64IM advertises only `FE_TONEAREST`, with `FLT_ROUNDS == 1`
and `FE_ALL_EXCEPT == 0`. There are no exception-flag macros, mutable rounding
modes or hardware FP registers in this profile. Saving, holding, restoring and
updating the one non-stop environment all succeed. This is the C11 §7.6
library interface, not an Annex F or frontend `FENV_ACCESS` claim.

## Floating text conversion

`src/strtofp.c` converts directly to binary32, binary64, AMD64 x87 extended
or AArch64/RV64IM binary128 using integer operations and bounded stack
storage. It recognizes the ASCII `C`
locale (`POSIX` is an alias), including `.` as the radix character. The
optional `nan(...)` payload is consumed when syntactically valid but does not
affect the quiet NaN representation. End pointers refer to the original
narrow or wide string. `atof` uses the same conversion as `strtod`.

The parser rounds to nearest, ties to even by default, and honors AMD64's
x87 rounding direction for `long double`, SSE direction for `float` and
`double`, and the AArch64 FPCR direction. The soft-float RV64IM profile only
supports nearest-even. The parser does not explicitly raise floating-point
exceptions. It sets `ERANGE` on overflow and on
inexact results rounded to a subnormal or zero, but not for exact subnormals,
literal infinities, or a failed match. The target-compiler ABI exposes
`strtold` and `wcstold`; native AMD64, AArch64 and RV64IM Cephyr expose both in the standard headers.
Narrow and wide `scanf` use the same converter for `%L` floating fields.
All three native profiles can pass their `long double *` destination.

## Other boundaries

- `errno` is process-global, not thread-local.
- Stream state and exit-handler lists have no synchronization and are intended
  for single-threaded use.
- Startup does not run ELF init/fini arrays, TLS initialization, C++
  constructors, or dynamic-loader finalizers.
- The allocator does not promise hardened invalid/double-free detection.
- The runtime implements no threading or cancellation layer.
