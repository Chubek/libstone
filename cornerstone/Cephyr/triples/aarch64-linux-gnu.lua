-- Little-endian ELF AAPCS64, not ILP32, Windows, or Darwin.
return {
    triple = "aarch64-linux-gnu",
    architecture = "AArch64",
    abi = "AAPCS64",
    os = "linux",
    specification = "@archon/stdspec/aarch64/root.arch",
    assembler = { "clang" },
    assemble_flags = { "--target=aarch64-linux-gnu", "-march=armv8-a" },
    linker = { "ld.lld" },
    link_flags = { "-m", "aarch64linux" },
    archiver = { "llvm-ar" },
}
