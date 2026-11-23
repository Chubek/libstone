-- ELF SysV AMD64, not the x32 or Windows ABI.
return {
    triple = "x86_64-linux-gnu",
    architecture = "AMD64",
    abi = "SysVAMD64",
    os = "linux",
    specification = "@archon/stdspec/amd64/root.arch",
    assembler = { "@package/sandstone-as" },
    assemble_flags = { "--bank", "@bank/amd64.symas", "-f", "elf", "-s", "att" },
    linker = { "@package/qobjld" },
    link_flags = { "-m", "elf_x86_64" },
    archiver = { "ar" },
}
