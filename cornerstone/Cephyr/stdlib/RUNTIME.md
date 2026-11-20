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
`abort`. Kernel errors become `-1` and update `errno`.

`src/runtime.c` provides `atexit` and `exit`. It supports up to 32 handlers.
Stdio can define `__cephyr_stdio_flush_all` to have `exit` flush buffered
streams.

`src/allocator.c` provides `malloc`, `calloc`, `realloc`, and `free` using one
anonymous mapping per allocation. It is intentionally simple rather than an
arena allocator.

Current boundaries:

- `errno` is process-global, not thread-local.
- `atexit` has no synchronization and is intended for single-threaded use.
- Startup does not run ELF init/fini arrays, TLS initialization, C++
  constructors, or dynamic-loader finalizers.
- The allocator does not promise hardened invalid/double-free detection.
- The runtime implements no threading or cancellation layer.
