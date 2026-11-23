-- Little-endian ELF AAPCS64, not ILP32, Windows, or Darwin.
return {
    triple = "aarch64-linux-gnu",
    architecture = "AArch64",
    abi = "AAPCS64",
    os = "linux",
    specification = "@archon/stdspec/aarch64/root.arch",
    assembler = { "@package/sandstone-as" },
    assemble_flags = { "--bank", "@bank/aarch64.symas", "-f", "elf" },
    linker = { "@package/qobjld" },
    link_flags = { "-m", "aarch64linux" },
    archiver = { "ar" },
}
