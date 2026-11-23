-- Bare-metal alternative. It deliberately uses the same reviewed native
-- RISCV64IM/LP64 backend but marks os = "none", so Linux libc is rejected.
return {
    triple = "riscv64-unknown-elf",
    architecture = "RISCV64IM",
    abi = "LP64",
    os = "none",
    specification = "@archon/stdspec/riscv/rv64im.arch",

    assembler = { "@package/sandstone-as" },
    assemble_flags = { "--bank", "@bank/rv64im.symas", "-f", "elf" },
    linker = { "@package/qobjld" },
    link_flags = { "--format=elf" },
    archiver = { "ar" },
}
