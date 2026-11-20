-- ELF SysV AMD64, not the x32 or Windows ABI.
return {
    triple = "x86_64-linux-gnu",
    architecture = "AMD64",
    abi = "SysVAMD64",
    os = "linux",
    specification = "@archon/stdspec/amd64/root.arch",
    assembler = { "clang" },
    assemble_flags = { "--target=x86_64-linux-gnu" },
    linker = { "ld.lld" },
    link_flags = { "-m", "elf_x86_64" },
    archiver = { "llvm-ar" },
}
