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

Current boundaries:

- `errno` is process-global, not thread-local.
- Stream state and exit-handler lists have no synchronization and are intended
  for single-threaded use.
- Startup does not run ELF init/fini arrays, TLS initialization, C++
  constructors, or dynamic-loader finalizers.
- The allocator does not promise hardened invalid/double-free detection.
- The runtime implements no threading or cancellation layer.
