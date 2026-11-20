return {
    triple = "x86_64-cephyrsdk-linux-gnu",
    -- SDK-only architecture; the test callback deliberately emits x86-64 GAS.
    architecture = "Example64",
    abi = "LP64",
    os = "linux",
    specification = "@archon/stdspec/amd64/root.arch",
    assembler = { "clang" },
    assemble_flags = { "--target=x86_64-linux-gnu" },
    linker = { "ld.lld" },
    link_flags = { "-m", "elf_x86_64" },
    archiver = { "llvm-ar" },
}
