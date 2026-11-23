return {
    triple = "x86_64-cephyrsdk-linux-gnu",
    -- SDK-only architecture; the test callback deliberately emits x86-64 GAS.
    architecture = "Example64",
    abi = "LP64",
    os = "linux",
    specification = "@archon/stdspec/amd64/root.arch",
    assembler = { "@package/sandstone-as" },
    assemble_flags = { "--bank", "@bank/amd64.symas", "-s", "att", "-f", "elf" },
    linker = { "@package/qobjld" },
    link_flags = { "-m", "elf_x86_64" },
    archiver = { "ar" },
}
