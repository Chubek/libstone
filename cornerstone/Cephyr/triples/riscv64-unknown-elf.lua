-- Bare-metal alternative. It deliberately uses the same reviewed native
-- RISCV64IM/LP64 backend but marks os = "none", so Linux libc is rejected.
return {
    triple = "riscv64-unknown-elf",
    architecture = "RISCV64IM",
    abi = "LP64",
    os = "none",
    specification = "@archon/stdspec/riscv/rv64im.arch",

    assembler = { "clang" },
    assemble_flags = {
        "--target=riscv64-unknown-elf",
        "-march=rv64im",
        "-mabi=lp64",
    },
    linker = { "ld.lld" },
    link_flags = { "-m", "elf64lriscv" },
    archiver = { "llvm-ar" },
}
