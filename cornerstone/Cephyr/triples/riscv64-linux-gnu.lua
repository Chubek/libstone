-- Cephyr's reviewed native backend is RISCV64IM/LP64. This is data evaluated
-- in a restricted Lua environment; commands are argv arrays, never shell text.
return {
    triple = "riscv64-linux-gnu",
    architecture = "RISCV64IM",
    abi = "LP64",
    os = "linux",
    specification = "@archon/stdspec/riscv/rv64im.arch",

    assembler = { "clang" },
    assemble_flags = {
        "--target=riscv64-linux-gnu",
        "-march=rv64im",
        "-mabi=lp64",
    },
    linker = { "ld.lld" },
    link_flags = { "-m", "elf64lriscv" },
    archiver = { "llvm-ar" },
}
