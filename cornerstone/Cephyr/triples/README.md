# Cephyr target bank

Each target is a Lua text chunk returning one table with these required fields:

- strings: `triple`, `architecture`, `abi`, `os`, `specification`;
- non-empty string arrays: `assembler`, `linker`, `archiver`;
- string arrays: `assemble_flags`, `link_flags`.

`os` is either `linux` or `none`. `specification` is an absolute path, a path
relative to the target script, or `@archon/...` relative to the `archon_root`
passed to `load_target`. Commands and flags are argv entries and are never
evaluated by a shell.

The command arrays may contain fixed arguments after the executable.
`assemble_flags` contains target/ISA options only; the driver appends its
compile mode, language, input, and output arguments. `link_flags` similarly
contains target linker-emulation options, while the driver owns link mode,
inputs, and output. TOML tool entries replace the corresponding target command;
TOML flag entries are always appended. If you replace a tool, its Lua flags are
not inherited: supply any required target options in the replacement command
or TOML flags.

The Lua state has a memory limit, an instruction limit, and no native
file/process libraries. `script_dir` and `archon_root` are the only environment
values. Target scripts are still configuration code and must be treated as
trusted project or installation data, not as a general security boundary.

Cephyr uses Archon's reviewed `AMD64`/`SysVAMD64`, `AArch64`/`AAPCS64`, and
`RISCV64IM`/`LP64` backends. The default triple is `x86_64-linux-gnu`.

## Automatic selection

| Host machine | OS / environment | Process ABI | Required ISA | Bank entry |
| --- | --- | --- | --- | --- |
| `x86_64` (little-endian) | Linux / GNU (glibc) | SysV LP64 | baseline AMD64 | `x86_64-linux-gnu.lua` |
| `aarch64` (little-endian) | Linux / GNU (glibc) | AAPCS64 LP64 | Armv8-A | `aarch64-linux-gnu.lua` |
| `riscv64` (little-endian) | Linux / GNU (glibc) | LP64 (soft float) | I and M | `riscv64-linux-gnu.lua` |

Auto loads the named entry from the configured bank and checks its Archon
architecture, ABI, OS, and specification against the reviewed provider. An
arbitrary Lua file, a bare-metal entry, or an Archon classic/VM generator
artifact cannot enable host selection. Multiple reviewed matches are an
error, not a filename-order preference.

To enable another host, first integrate and test Cephyr C lowering and Archon's
native backend together for the ISA and ABI, with the relevant tools and
runtime. Then register the reviewed stdspec/provider and host mapping in
`src/host_target.cpp` and add the corresponding Lua bank entry. New Archon
stdspec or IRGen support alone does not activate auto selection or change the
default.
