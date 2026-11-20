# Native backend reference client

`client.c` uses only the public `Backend.h` C ABI. It builds and compiles
two independent in-memory MachineIR modules in a single host process. The
first emits `choose_max`, with block arguments selecting the result of a
signed comparison. The second emits `scale_max`, which imports both
`choose_max` and a function defined by the target C program. The two
assembly outputs are assembled separately and linked with the target C
program using the RV64IM LP64 ABI.

## Build and run

From `cornerstone/Archon`, with a host C compiler, CMake, Clang with
RISC-V support, and LLD:

```sh
work=$(mktemp -d /tmp/opencode/archon-native-example.XXXXXX)
export work
python - <<'PY'
import os
from pathlib import Path

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory

db = compile_file(Path("stdspec/riscv/rv64im.arch"))
files = generate_ir("gated", [db], options={
    "profile": "native", "abi": "LP64", "oracle": False,
})
write_generated_directory(files, Path(os.environ["work"]) / "generated")
PY

cmake -S "$work/generated" -B "$work/build"
cmake --build "$work/build" --target archon_backend
cc -std=c99 -I "$work/generated" examples/native-backend/client.c \
    "$work/build/libarchon_backend.a" -o "$work/client"
"$work/client" "$work/choice.s" "$work/scale.s"

rvcc() { clang --target=riscv64-linux-gnu -march=rv64im -mabi=lp64 "$@"; }
rvcc -c "$work/choice.s" -o "$work/choice.o"
rvcc -c "$work/scale.s" -o "$work/scale.o"
rvcc -c examples/native-backend/start.S -o "$work/start.o"
rvcc -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
    -c examples/native-backend/target.c -o "$work/target.o"
ld.lld -m elf64lriscv -static -e _start -o "$work/demo" \
    "$work/start.o" "$work/target.o" "$work/choice.o" "$work/scale.o"
qemu-riscv64 "$work/demo"
```

The freestanding executable exits with status 0 on success and needs no
RISC-V libc or sysroot.

To emit raw code without invoking a target assembler, build the generated
driver and compile the [factorial](factorial.mir) example:

```sh
cmake --build "$work/build" --target archon-backend
"$work/build/archon-backend" --input examples/native-backend/factorial.mir \
    --emit-code >"$work/factorial.bin"
```

Its entry is at offset zero. Inputs from 0 through 20 give a factorial that
fits in signed `i64`. The function expects the usual LP64 stack alignment and
return address. This file is not an ELF executable;
`tests/test_native_machine_code.py` loads it in an RV64 emulator.

## Embedding contract

Populate a versioned `ArchonMIRModule` and keep its backing tables, strings,
and options valid and unchanged until `archon_backend_compile` returns.
Pass a zero-initialized artifact, write exactly the returned `size` bytes,
and release it with `archon_backend_artifact_destroy`. An unsuccessful call
reports status and, when requested, a diagnostic without publishing a
partial artifact. The output is assembly, not an object file or executable
memory. The caller supplies the assembler, linker, and any imported symbols.

The additional fixtures `test_client.c` and `test_target.c` exercise loop
block arguments, Euclidean GCD against a C reference, array summation,
recursive calls, stack arguments, large frame offsets, memory and register
spilling, checked arithmetic traps, malformed MIR, and repeated compilation.
Run `python3 -m unittest discover -s tests -p test_irgen_native.py` to build
and test them. Execution uses `qemu-riscv64` (or the executable named by
`ARCHON_QEMU_RISCV64`), falling back to Unicorn with RISC-V support if
available. Without either emulator, the test still assembles and links
the RV64 images.
