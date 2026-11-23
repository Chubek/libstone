-- Cephyr's reviewed native backend is RISCV64IM/LP64. This is data evaluated
-- in a restricted Lua environment; commands are argv arrays, never shell text.
return {
    triple = "riscv64-linux-gnu",
    architecture = "RISCV64IM",
    abi = "LP64",
    os = "linux",
    specification = "@archon/stdspec/riscv/rv64im.arch",

    assembler = { "@package/sandstone-as" },
    assemble_flags = { "--bank", "@bank/rv64im.symas", "-f", "elf" },
    linker = { "@package/qobjld" },
    link_flags = { "--format=elf" },
    archiver = { "ar" },
}
