// qABI.hpp - calling-convention view over qBFD Format/Arch.
//
// dyninvoke (tourchstone) consumes this to select a call ABI from an object
// file without reparsing headers. Header-only like the rest of qBFD.
#pragma once

#include "qBFDCore.hpp"

namespace qbfd::abi {

// Calling conventions dyninvoke can lower. Values mirror dinv_abi so the
// mapping stays obvious across the tree.
enum class CallConv : uint8_t {
  Unknown = 0,
  SysV_AMD64 = 1,  // ELF amd64, Mach-O x86_64
  Win64 = 2,       // PE amd64
  Aapcs64 = 3,     // ELF/Mach-O/PE aarch64
  Riscv64 = 4,     // ELF/PE riscv64 (LP64D)
  Riscv32 = 5,     // ELF riscv32 (ILP32D)
};

struct AbiInfo {
  CallConv conv = CallConv::Unknown;
  const char* name = "unknown";      // "sysv_amd64", "win64", ...
  const char* isaArch = "";          // sandstone arch block name
  const char* isaFile = "";          // sandstone file under sandstone/isa/
  int gprArgRegs = 0;                // registers available for args
  int fprArgRegs = 0;
  int stackAlign = 16;
  bool hiddenRetPtr = true;  // large struct returns use a hidden pointer
};

inline std::string_view toString(CallConv c) {
  switch (c) {
    case CallConv::SysV_AMD64: return "sysv_amd64";
    case CallConv::Win64: return "win64";
    case CallConv::Aapcs64: return "aapcs64";
    case CallConv::Riscv64: return "riscv64";
    case CallConv::Riscv32: return "riscv32";
    default: return "unknown";
  }
}

inline AbiInfo infoFor(CallConv conv) {
  switch (conv) {
    case CallConv::SysV_AMD64:
      return {conv, "sysv_amd64", "amd64", "x86.isa", 6, 8, 16, true};
    case CallConv::Win64:
      return {conv, "win64", "amd64", "x86.isa", 4, 4, 16, true};
    case CallConv::Aapcs64:
      return {conv, "aapcs64", "aarch64", "aarch64.isa", 8, 8, 16, true};
    case CallConv::Riscv64:
      return {conv, "riscv64", "riscv64", "riscv64.isa", 8, 8, 16, true};
    case CallConv::Riscv32:
      return {conv, "riscv32", "riscv32", "riscv32.isa", 8, 8, 16, true};
    default:
      return {};
  }
}

// Format-aware selection. PE amd64 uses Win64; ELF/Mach-O amd64 use SysV.
// AArch64 is AAPCS64 on all three formats (Apple/Windows variants share the
// register assignment; only stack-home and va_list details differ).
inline CallConv convFor(Format format, Arch arch) {
  switch (arch) {
    case Arch::X86_64:
      if (format == Format::PE || format == Format::COFF) return CallConv::Win64;
      if (format == Format::ELF || format == Format::MachO) return CallConv::SysV_AMD64;
      return CallConv::Unknown;
    case Arch::AArch64:
      if (format == Format::ELF || format == Format::MachO || format == Format::PE ||
          format == Format::COFF)
        return CallConv::Aapcs64;
      return CallConv::Unknown;
    case Arch::RISCV64:
      if (format == Format::ELF || format == Format::PE || format == Format::COFF)
        return CallConv::Riscv64;
      return CallConv::Unknown;
    case Arch::RISCV32:
      if (format == Format::ELF || format == Format::PE || format == Format::COFF)
        return CallConv::Riscv32;
      return CallConv::Unknown;
    default:
      return CallConv::Unknown;
  }
}

inline AbiInfo infoFor(Format format, Arch arch) {
  return infoFor(convFor(format, arch));
}

inline std::string_view isaArchFor(CallConv c) { return infoFor(c).isaArch; }
inline std::string_view isaFileFor(CallConv c) { return infoFor(c).isaFile; }

// Host default used for DINV_ABI_AUTO.
inline CallConv hostConv() {
#if defined(_WIN32) && defined(__x86_64__)
  return CallConv::Win64;
#elif defined(__x86_64__)
  return CallConv::SysV_AMD64;
#elif defined(__aarch64__)
  return CallConv::Aapcs64;
#elif defined(__riscv) && __riscv_xlen == 64
  return CallConv::Riscv64;
#elif defined(__riscv) && __riscv_xlen == 32
  return CallConv::Riscv32;
#else
  return CallConv::Unknown;
#endif
}

}  // namespace qbfd::abi
