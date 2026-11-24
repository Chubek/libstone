# AMD64 encoder coverage

`stdspec/amd64.dsymspec` describes **x86-64 long mode, SysV LP64 / ELF64**. It is
not a catalog of all x86 encodings. References are the [AMD64 Architecture
Programmer's Manual, volumes 3–5](https://www.amd.com/en/search/documentation/hub.html),
[Intel SDM, volume 2](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
and [x86-64 psABI, §4.4](https://gitlab.com/x86-psABIs/x86-64-ABI). LLVM MC
22.1.8 is the independent encoding and ELF reference for the fixture.

## Running the fixture

```sh
# First build sandstone-dsymspec as shown in sandstone/dsymspec/README.md.
mkdir -p build/amd64
TMPDIR="$PWD/build/amd64" cc -std=c11 -pedantic \
  -I build/dsymspec/dsymspec/generated -I sandstone/dsymspec \
  sandstone/dsymspec/tests/amd64/encode.c -o build/amd64/encode
python3 sandstone/dsymspec/tests/amd64/differential.py build/amd64/encode --work build/amd64
```

The fixture runs real generated C recipes. It compares instruction bytes with
`llvm-mc`, instruction relocation records and placeholders with
`llvm-readobj`/`llvm-objcopy`, and checks that invalid operands fail without
committing any output. It enumerates the integer ALU register/memory/immediate
matrix and FMA3 orders, and samples the other feature strata. These tests do not
execute the resulting instructions or test what any particular CPU enables.

## Normalized bindings for the `x64_…` encoders

- Operands use encoding order (destination first). `rN` is the encoding number
  of register operand N, beginning at zero. In byte GPR operands only,
  **16..19** denote `ah`, `ch`, `dh`, `bh`; these are tags, not APX register
  numbers. They map to hardware codes 4..7 without REX. The real REX-byte
  registers `spl`, `bpl`, `sil`, `dil` use 4..7. Legacy/VEX vector forms reject
  encodings above 15; EVEX forms can use 0..31.
- Memory operand N supplies `modrmN` (ModR/M with only `mod` and `r/m` filled),
  `sibN` (-1 if absent, otherwise an encoded byte), and `bN`/`xN` (zero or one).
  Add 256 to `modrmN` for 32-bit addressing, 512 for FS, or 1024 for GS.
  `dsN` gives the displacement field size (0, 1 or 4 bytes). `dispN` is the
  signed, **unscaled** displacement, even for EVEX compressed disp8.
- `fixN` is 0 for numeric displacement. When a symbol is present, `refN` is its
  name and `dispN` the source addend. Fixups 1/2 are RIP/EIP-relative
  PC32/GOTPCREL, 3 absolute 32/32S (address-size dependent), and 4 TPOFF32.
  The 64-bit MOV/ADD load forms also accept 5 GOTTPOFF; 64-bit LEA accepts
  6 TLSGD, 7 TLSLD and 8 GOTPC32_TLSDESC. The PC bias includes trailing
  immediate bytes. A standalone resolved branch `imm` is already relative to
  the instruction end; a symbolic branch uses `sym` and `addend` instead.
- `imm` is the normalized immediate or resolved relative displacement.
  `imm2` is the second immediate for ENTER. `offset` is the absolute offset
  for accumulator `moffs` MOV forms. `mask` is required for maskable EVEX
  forms, 0 for no mask, 1..7 for merge, or 9..15 for zeroing. Zeroing stores
  and zeroing without a real mask are rejected. `broadcast` in a form record
  identifies the lane replication count of the encoded memory operand.
- The encoder does not select forms, apply relocations, relax branches, or
  parse prefixes. A frontend must inspect each form's `required`, `prefix`,
  `indirect`, `address_size`, `masking`, `broadcast`, `syntaxes` and operand
  fields as applicable and normalize its syntax accordingly. Older encoders
  retain their earlier binding conventions. The legacy SandAS/SymAs
  parsers do not gain syntax merely by building the standalone `.i` profile.

## Inventory and limits

| Stratum | Covered here |
| --- | --- |
| Base integer | ALU, immediate moves, sign/zero extension, LEA, multiply/divide, bit scans/tests, shifts/rotates, exchange/atomic families, the condition-code matrix, stack, near transfers, far memory transfers/returns, ports, string/REP families and segment transfers |
| Base system and `priv` | Descriptor/status operations, control/debug moves, traps, system calls/returns, cache/fences, FXSAVE/FXRSTOR, common VMX/SVM forms and optional CX16, RDTSCP, MONITOR and LAHF/SAHF |
| `sse`, `sse2`, `mmx`, `x87` | Scalar/packed floating arithmetic, moves, comparisons and conversions; packed integer arithmetic/logic/shift families; x87 stack arithmetic, comparisons, constants, transcendental and typed memory/state families |
| `sse3`, `ssse3`, `sse4.1`, `sse4.2` | Horizontal/duplicate moves and FISTTP, SSSE3 packed operations, SSE4 widening/extract/insert, min/max, rounding/blends/dot products, packed string compare and CRC32 |
| `avx`, `avx2`, `fma` | VEX.128/256 floating/vector integer families, broadcast, permutations, extraction/insertion, variable shifts, zeroing, and FMA3 precision/order families |
| `avx512` / `avx512vl`, `avx512bw`, `avx512dq` | Selected EVEX arithmetic, logical and move families, predicate registers, high registers, merge/zero masking, broadcast tuples, compressed displacements and vector crypto |
| `bmi1`, `bmi2`, `adx`, `popcnt`, `lzcnt`, `movbe`, `fsgsbase` | Scalar register and relevant memory/size variants |
| `aes`, `vaes`, `pclmulqdq`, `vpclmulqdq`, `sha`, `gfni` | AES rounds, inverse mix and keygen, carry-less multiply, SHA1/SHA256 and GFNI; relevant legacy, VEX and EVEX forms |
| `rdrnd`, `rdseed`, `rdpid`, `clflushopt`, `clwb`, `cldemote`, `clzero`, `movdiri`, `waitpkg`, `serialize` | Individual register, memory and fixed forms |
| `cet`, `rtm`, `sgx`, `uintr` | ENDBR and shadow-stack instructions, RTM control, SGX leaf dispatch, and user interrupt instructions |
| ELF | Standard LP64 relocation catalog (including large-model GOT/PLT and TLS/relaxation types); only the fixups listed above and absolute symbol move/branch forms are emitted from instruction recipes |

Important remaining gaps:

- Alternative aliases, redundant and vendor-specific prefixes, HLE/MPX,
  alternate noncanonical opcode encodings, x87 compatibility spellings, and
  some specialized integer/system forms (XSAVE family, INVPCID, VMFUNC,
  protection-key/MSR extensions). No compatibility/legacy execution modes.
- Remaining SSE/MMX conversion and extension forms; part of AVX/AVX2
  conversions/shuffles, gather/VSIB, mask-moves and comparisons. EVEX does not
  yet cover the full AVX-512F/BW/DQ/VL matrices, SAE/rounding, VSIB
  gather/scatter, or the other AVX-512 subsets. No APX, AVX10, AMX, XOP,
  FMA4, 3DNow!, or implementation-specific instruction inventory.
- No profile-wide feature implication or architectural state checking
  (CPL, CR4, XCR0, CPUID, memory alignment). `cet` is a catalog umbrella for
  IBT and SHSTK, not independent CPUID discovery.
- No layout, GNU/Intel parser completeness, relaxable GOTPCRELX emission
  selection, TLS descriptor call marker emission, or complete symbolic
  immediate-relocation policy. These are distinct from relocation records
  being present in the catalog.
