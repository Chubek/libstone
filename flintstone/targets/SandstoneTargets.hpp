#pragma once
#include "Native.hpp"

namespace flintstone {

// Metadata for every architecture shipped in sandstone/compiler/stdspec.
// Keys match the .dsymspec filenames; banks themselves are compiled on demand
// with the Sandstone spec compiler, not embedded here.
struct ArchInfo {
    std::string key, arch, subarch, abi, obj_format, endian;
    unsigned addr_size = 0;
};
std::vector<ArchInfo> all_architectures();
const ArchInfo* find_architecture(std::string_view key);

// Tiny but real integer subsets with verified machine encodings. Each bank
// provides GPRs r0..r7 (the return register is r0), li/add/sub/mul/mov/ret
// forms, one scheduling model and concrete timing. The selector supports
// straight-line i32 integer functions (constant/copy/add/sub/mul/return);
// anything else fails explicitly with tiny.* diagnostics.
enum class TinyArch { X86_64, AArch64, RiscV64 };
// The canonical target name for an architecture and its inverse. The mapping
// is defined once so the CLI, the backend registry and the published name
// list cannot drift apart.
std::string tiny_arch_name(TinyArch);
bool find_tiny_arch(std::string_view name, TinyArch& arch);
struct TinyForms {
    std::uint32_t li = 0, add = 0, sub = 0, mul = 0, mov = 0, ret = 0, model = 0;
};
std::shared_ptr<domsymas::Databank> make_tiny_bank(TinyArch, TinyForms* forms = nullptr);
NativeTarget make_tiny_target(TinyArch);
std::vector<std::string> tiny_backend_names(); // {"x86-64-tiny","aarch64-tiny","riscv64-tiny"}
void register_tiny_backends(Registry&);

} // namespace flintstone
