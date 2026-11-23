// relocate.cpp - relocation application.
//
// Supports the x86/x64 static relocations the fd readers decode:
//   R_X86_64_64 / R_X86_64_32 / R_X86_64_32S (absolute)
//   R_X86_64_PC32 / R_X86_64_PLT32 (PC-relative; PLT32 relaxes to PC32 in a
//     static link since no PLT is emitted)
//   R_386_32 / R_386_PC32
//   R_X86_64_RELATIVE is never present in ET_REL inputs; it is rejected.
//
// Other types are reported as errors when they occur in an allocated
// section, and ignored in debug sections (which are dropped from the
// executable image when --strip-debug, or copied verbatim otherwise).
#include "internal.hpp"

namespace qld::detail {

namespace {

void writeLE(std::vector<uint8_t>& buf, uint64_t off, uint64_t value,
             unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    buf[off + i] = uint8_t(value >> (8 * i));
}

void writeBE(std::vector<uint8_t>& buf, uint64_t off, uint64_t value,
             unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    buf[off + i] = uint8_t(value >> (8 * (width - i - 1)));
}

void writeVal(std::vector<uint8_t>& buf, uint64_t off, uint64_t value,
              unsigned width, qbfd::Endian order) {
  if (order == qbfd::Endian::Little)
    writeLE(buf, off, value, width);
  else
    writeBE(buf, off, value, width);
}

uint64_t readVal(const std::vector<uint8_t>& buf, uint64_t off, unsigned width,
                 qbfd::Endian order) {
  uint64_t v = 0;
  if (order == qbfd::Endian::Little) {
    for (unsigned i = 0; i < width; ++i) v |= uint64_t(buf[off + i]) << (8 * i);
  } else {
    for (unsigned i = 0; i < width; ++i) v = (v << 8) | buf[off + i];
  }
  return v;
}

}  // namespace

bool applyRelocations(LinkState& state,
                      std::vector<std::vector<uint8_t>>& staged,
                      std::string& error) {
  // staged[i] parallels state.outSections[i]: mutable section bytes.
  if (staged.size() != state.outSections.size()) {
    error = "internal error: staged image size mismatch";
    return false;
  }
  bool isX86_64 = state.arch == qbfd::Arch::X86_64;
  bool isX86 = state.arch == qbfd::Arch::X86;
  bool isAArch64 = state.arch == qbfd::Arch::AArch64;
  bool isRISCV = state.arch == qbfd::Arch::RISCV32 ||
                 state.arch == qbfd::Arch::RISCV64;
  bool isARM = state.arch == qbfd::Arch::ARM;
  bool isPPC = state.arch == qbfd::Arch::PowerPC ||
               state.arch == qbfd::Arch::PowerPC64;
  bool relocatable = state.options.mode == Mode::Relocatable;
  const bool strictSections = state.ltoPlan.gcSections;

  std::map<uint64_t, uint64_t> rvHigh;
  size_t gotSection = SIZE_MAX;
  if (state.staticGotInput) {
    for (size_t i = 0; i < state.outSections.size(); ++i)
      if (state.outSections[i].name == ".got") gotSection = i;
  }
  // The LO12 relocation names the address of the AUIPC, not its symbol.
  // Resolve all high halves first, independent of section/relocation order.
  for (int pass = isRISCV && !relocatable ? 0 : 1; pass != 2; ++pass) {
  for (const auto& in : state.inputSections) {
    if (!in.kept) continue;
    if (in.relocs.empty()) continue;
    // Output section + base for this input.
    size_t outIdx = SIZE_MAX;
    for (size_t o = 0; o < state.outSections.size(); ++o) {
      for (size_t idx : state.outSections[o].inputs) {
        if (state.inputSections[idx].objectIndex == in.objectIndex &&
            state.inputSections[idx].sectionIndex == in.sectionIndex) {
          outIdx = o;
          break;
        }
      }
      if (outIdx != SIZE_MAX) break;
    }
    if (outIdx == SIZE_MAX) {
      if (strictSections) {
        error = "internal error: live relocation section has no output placement";
        return false;
      }
      continue;
    }
    const OutputSection& o = state.outSections[outIdx];
    const auto& obj = state.objects[in.objectIndex];
    uint64_t place = o.vma + in.outOffset;  // P for PC-relative
    std::vector<uint8_t>& img = staged[outIdx];
    uint64_t baseOff = in.outOffset;

    for (const auto& r : in.relocs) {
      if (isRISCV && (r.nativeType == 0 || r.nativeType == 51)) continue;
      if (isRISCV && r.nativeType == 43 && !relocatable) {
        // Retain emitted NOP padding only if it already aligns the following instruction.
        if (r.addend < 0 || r.addend >= INT32_MAX || r.offset > in.section.size ||
            uint64_t(r.addend) > in.section.size - r.offset) {
          error = obj.label + ": invalid R_RISCV_ALIGN padding"; return false;
        }
        uint64_t alignment = 1;
        while (alignment <= uint64_t(r.addend)) alignment <<= 1;
        if ((place + r.offset + r.addend) % alignment) {
          error = obj.label + ": R_RISCV_ALIGN requires instruction relaxation; assemble with -mno-relax"; return false;
        }
        continue;
      }
      if (pass == 0 && r.nativeType != 23 && r.nativeType != 20) continue;
      // Resolve the symbol value S.
      uint64_t S = 0;
      std::string symName;
      bool haveSym = false;
      if (r.symbol) {
        const auto& syms = obj.object->symbols();
        const qbfd::Symbol* sym = nullptr;
        if (*r.symbol < syms.size()) sym = &syms[*r.symbol];
        if (!sym) {
          error = obj.label + ": relocation names bad symbol";
          return false;
        }
        symName = sym->name;
        if (sym->flags & qbfd::sym::Undefined) {
          auto it = state.symAddr.find(sym->name);
          if (it == state.symAddr.end()) {
            if (sym->binding == qbfd::SymbolBinding::Weak || state.options.allowUndefined ||
                state.options.mode == Mode::Relocatable) {
              S = 0;
            } else {
              error = obj.label + ": undefined reference to `" + sym->name +
                      "'";
              return false;
            }
          } else {
            S = it->second;
          }
          haveSym = true;
        } else if (sym->kind == qbfd::SymbolKind::Section ||
                   sym->kind == qbfd::SymbolKind::File) {
          // Section symbol: S = owning output section base + value.
          if (sym->section) {
            bool found = false;
            for (size_t oi2 = 0; oi2 < state.outSections.size(); ++oi2) {
              for (size_t idx : state.outSections[oi2].inputs) {
                const auto& other = state.inputSections[idx];
                if (other.objectIndex == in.objectIndex &&
                    other.sectionIndex == *sym->section) {
                  S = state.outSections[oi2].vma + other.outOffset +
                      sym->value;
                  found = true;
                  break;
                }
              }
              if (found) break;
            }
            if (!found) {
              if (strictSections) {
                error = obj.label + ": live relocation refers to an unplaced section";
                return false;
              }
              S = sym->value;
            }
          } else {
            S = sym->value;
          }
          haveSym = true;
        } else {
          auto it = usesGlobalRelocationSymbol(*sym)
                        ? state.symAddr.find(sym->name) : state.symAddr.end();
          if (it != state.symAddr.end()) {
            S = it->second;
          } else {
            // Local symbol not in the global table: compute directly.
            if (sym->section) {
              bool found = false;
              for (size_t oi2 = 0; oi2 < state.outSections.size(); ++oi2) {
                for (size_t idx : state.outSections[oi2].inputs) {
                  const auto& other = state.inputSections[idx];
                  if (other.objectIndex == in.objectIndex &&
                      other.sectionIndex == *sym->section) {
                    S = state.outSections[oi2].vma + other.outOffset +
                        sym->value;
                    found = true;
                    break;
                  }
                }
                if (found) break;
              }
              if (!found) {
                if (strictSections) {
                  error = obj.label + ": live relocation refers to an unplaced definition of `" +
                          symName + "'";
                  return false;
                }
                S = sym->value;
              }
            } else {
              S = sym->value;
            }
          }
          haveSym = true;
        }
      }
      (void)haveSym;
      int64_t A = r.hasAddend ? r.addend : 0;
      // Implicit addends for REL inputs are already in the section bytes;
      // qELF decodes them into addend when the type is recognised, so we
      // must not double-count: when hasAddend came from decoding (REL),
      // the bytes still hold the original value and the formula below
      // overwrites them, which is correct.
      uint64_t P = place + r.offset;
      uint64_t loc = baseOff + r.offset;
      uint32_t type = r.nativeType;

      auto needBytes = [&](unsigned w) -> bool {
        if (r.offset > in.section.size || w > in.section.size - r.offset || r.offset > UINT64_MAX - baseOff || loc > img.size() || w > img.size() - loc) {
          error = obj.label + ": relocation offset outside section " +
                  in.section.name;
          return false;
        }
        return true;
      };
      // Combine addresses and signed addends in unsigned arithmetic, then
      // check signed field widths. Signed address arithmetic can overflow.
      auto signedAddend = [&](unsigned w) {
        if (r.hasAddend) return uint64_t(A);
        auto value = readVal(img, loc, w, state.endian);
        return w == 4 ? uint64_t(int64_t(int32_t(value))) : value;
      };
      auto fitsS32 = [](uint64_t v) { return v == uint64_t(int64_t(int32_t(v))); };

      if (relocatable) {
        // Partial link: leave relocations for later; nothing to apply.
        continue;
      }

      const bool x86Got = isX86_64 && (type == 9 || type == 41 || type == 42);
      if (x86Got || (isAArch64 && (type == 311 || type == 312)) || (isRISCV && type == 20)) {
        if (gotSection == SIZE_MAX || !r.symbol || (!x86Got && A) || !state.staticGot.count({in.objectIndex,*r.symbol})) {
          error = obj.label + ": static GOT relocation requires default layout and zero addend";
          return false;
        }
        const uint64_t off = state.inputSections[*state.staticGotInput].outOffset + state.staticGot.at({in.objectIndex,*r.symbol});
        writeVal(staged[gotSection], off, S, 8, state.endian);
        S = state.outSections[gotSection].vma + off;
      }
      const uint64_t addr = S + uint64_t(A), delta = addr - P;
      if (!pass) {
        if (!rvHigh.emplace(P, delta).second) { error = obj.label + ": ambiguous PCREL_HI20"; return false; }
        continue;
      }
      auto signedFits = [](uint64_t value, unsigned n) {
        const uint64_t half = UINT64_C(1) << (n-1);
        return value + half < 2 * half;
      };
      auto invalid = [&] { error = obj.label + ": misaligned or out-of-range relocation " + r.typeName + " against `" + symName + "'"; return false; };
      auto insn = [&](unsigned off = 0) { return uint32_t(readVal(img, loc + off, 4, state.endian)); };
      auto patch = [&](uint32_t mask, uint32_t value, unsigned off = 0) {
        writeVal(img, loc + off, (insn(off) & ~mask) | (value & mask), 4, state.endian);
      };
      if (isAArch64 && ((type >= 274 && type <= 280) || (type >= 282 && type <= 286) || type == 299 || type == 311 || type == 312)) {
        if (!needBytes(4)) return false;
        if (type == 274 || type == 275 || type == 276 || type == 311) {
          const uint64_t v = type == 274 ? delta : (addr >> 12) - (P >> 12);
          if (type != 276 && !signedFits(v,21)) return invalid();
          patch(0x60ffffe0, ((v & 3) << 29) | (((v >> 2) & 0x7ffff) << 5));
        } else if (type == 279 || type == 280 || type == 282 || type == 283) {
          unsigned bits = type == 279 ? 14 : type == 280 ? 19 : 26;
          if ((delta & 3) || !signedFits(delta, bits + 2)) return invalid();
          unsigned shift = bits == 26 ? 0 : 5;
          patch(((1u << bits)-1) << shift, (delta >> 2) << shift);
        } else {
          unsigned scale = type == 284 ? 1 : type == 285 ? 2 : type == 286 || type == 312 ? 3 : type == 299 ? 4 : 0;
          if (addr & ((1u << scale)-1)) return invalid();
          patch(0x003ffc00, ((addr & 4095) >> scale) << 10);
        }
        continue;
      }
      if (isRISCV && (type >= 16 && type <= 28) && type != 21 && type != 22) {
        if (!needBytes(type == 18 || type == 19 ? 8 : 4)) return false;
        uint64_t v = type >= 26 ? addr : delta;
        if (type == 24 || type == 25) {
          auto hi = rvHigh.find(S);
          if (hi == rvHigh.end() || A) { error = obj.label + ": unmatched PCREL_LO12 anchor"; return false; }
          v = hi->second;
        }
        if (type == 16) {
          if ((v & 1) || !signedFits(v,13)) return invalid();
          patch(0xfe000f80, ((v & 0x1000) << 19) | ((v & 0x7e0) << 20) | ((v & 0x1e) << 7) | ((v & 0x800) >> 4));
        } else if (type == 17) {
          if ((v & 1) || !signedFits(v,21)) return invalid();
          patch(0xfffff000, ((v & 0x100000) << 11) | ((v & 0x7fe) << 20) | ((v & 0x800) << 9) | (v & 0xff000));
        } else if (type == 24 || type == 27) patch(0xfff00000, v << 20);
        else if (type == 25 || type == 28) patch(0xfe000f80, ((v & 0xfe0) << 20) | ((v & 31) << 7));
        else {
          if (!signedFits((v + 0x800) & ~UINT64_C(0xfff),32)) return invalid();
          patch(0xfffff000, v + 0x800);
          if (type == 18 || type == 19) patch(0xfff00000, v << 20, 4);
        }
        continue;
      }
      if (isRISCV && type == 57) { // R_RISCV_32_PCREL
        if (!needBytes(4)) return false;
        if (!fitsS32(delta)) return invalid();
        writeVal(img, loc, delta, 4, state.endian);
        continue;
      }
      // GNU assemblers use paired ADD/SUB and SET/SUB relocations for
      // section differences in unwind information and jump tables.
      if (isRISCV && ((type >= 33 && type <= 40) || (type >= 52 && type <= 56))) {
        const unsigned width = type <= 40 ? 1u << ((type - 33) % 4) : type <= 54 ? 1 : 1u << (type - 54);
        if (!needBytes(width)) return false;
        const auto old = readVal(img, loc, width, state.endian);
        uint64_t value = type <= 36 ? old + addr : type <= 40 || type == 52 ? old - addr : addr;
        if (type == 52 || type == 53) value = (old & 0xc0) | (value & 0x3f);
        writeVal(img, loc, value, width, state.endian);
        continue;
      }
      if (isRISCV && type != 1 && type != 2) {
        if (!o.alloc) continue;
        error = obj.label + ": unsupported RISC-V relocation " + std::to_string(type);
        return false;
      }
      if (isX86_64) {
        switch (type) {
          case 9: case 41: case 42: // GOTPCREL[X], keep the load (no relaxation)
            if (!needBytes(4)) return false;
            if (!fitsS32(delta)) return invalid();
            writeVal(img, loc, delta, 4, state.endian);
            break;
          case 1: {  // R_X86_64_64
            if (!needBytes(8)) return false;
            uint64_t v = S + (uint64_t)(int64_t)A;
            // For REL (no explicit addend decoded), A already includes the
            // in-place value, but we overwrote? qELF sets hasAddend for
            // RELA always and for REL only when decoded. When not decoded
            // (unknown), we read the in-place value here.
            if (!r.hasAddend) {
              uint64_t inplace = readVal(img, loc, 8, state.endian);
              v = S + inplace;
            }
            writeVal(img, loc, v, 8, state.endian);
            break;
          }
          case 10: {  // R_X86_64_32
            if (!needBytes(4)) return false;
            uint64_t v = S + (uint64_t)(int64_t)A;
            if (!r.hasAddend) v = S + readVal(img, loc, 4, state.endian);
            if (v >> 32) {
              error = obj.label + ": relocation truncated to fit: " +
                      "R_X86_64_32 against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, v & 0xffffffffu, 4, state.endian);
            break;
          }
          case 11: {  // R_X86_64_32S
            if (!needBytes(4)) return false;
            uint64_t v = S + signedAddend(4);
            if (!fitsS32(v)) {
              error = obj.label + ": relocation truncated to fit: " +
                      "R_X86_64_32S against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, (uint64_t)(uint32_t)v, 4, state.endian);
            break;
          }
          case 2:   // R_X86_64_PC32
          case 4: {  // R_X86_64_PLT32 (relaxes to PC32 statically)
            if (!needBytes(4)) return false;
            uint64_t v = S + signedAddend(4) - P;
            if (!fitsS32(v)) {
              error = obj.label + ": relocation truncated to fit: " +
                      std::string(type == 2 ? "R_X86_64_PC32" : "R_X86_64_PLT32") +
                      " against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, (uint64_t)(uint32_t)v, 4, state.endian);
            break;
          }
          case 8: {  // R_X86_64_RELATIVE in ET_REL: unexpected
            error = obj.label + ": unsupported R_X86_64_RELATIVE in " +
                    "relocatable input";
            return false;
          }
          default: {
            // Unknown type in an alloc section is fatal; in debug, skip.
            if (o.alloc) {
              error = obj.label + ": unsupported relocation type " +
                      std::to_string(type) + " in " + in.section.name +
                      " against `" + symName + "'";
              return false;
            }
            break;
          }
        }
      } else if (isX86) {
        // 32-bit x86.
        switch (type) {
          case 1: {  // R_386_32
            if (!needBytes(4)) return false;
            uint64_t v = S + (uint64_t)(int64_t)A;
            if (!r.hasAddend) v = S + readVal(img, loc, 4, state.endian);
            writeVal(img, loc, v & 0xffffffffu, 4, state.endian);
            break;
          }
          case 2: {  // R_386_PC32
            if (!needBytes(4)) return false;
            uint64_t v = S + signedAddend(4) - P;
            writeVal(img, loc, (uint64_t)(uint32_t)v, 4, state.endian);
            break;
          }
          default: {
            if (o.alloc) {
              error = obj.label + ": unsupported relocation type " +
                      std::to_string(type) + " in " + in.section.name;
              return false;
            }
            break;
          }
        }
      } else if (isAArch64 || isRISCV || isARM || isPPC) {
        // Generic absolute/PC-relative relocations are deliberately handled
        // here without pretending to implement instruction-field relocations.
        // The latter require architecture-specific bit packing and are left
        // as explicit diagnostics instead of silently corrupting code.
        switch (type) {
          case 257: {  // R_AARCH64_ABS64
            if (!isAArch64) break;
            if (!needBytes(8)) return false;
            uint64_t v = S + uint64_t(A);
            if (!r.hasAddend) v = S + readVal(img, loc, 8, state.endian);
            writeVal(img, loc, v, 8, state.endian);
            break;
          }
          case 258: {  // R_AARCH64_ABS32
            if (!isAArch64) break;
            if (!needBytes(4)) return false;
            uint64_t v = S + uint64_t(A);
            if (!r.hasAddend) v = S + readVal(img, loc, 4, state.endian);
            if (v >> 32) {
              error = obj.label + ": relocation truncated to fit: R_AARCH64_ABS32";
              return false;
            }
            writeVal(img, loc, v, 4, state.endian);
            break;
          }
          case 260: {  // R_AARCH64_PREL64
            if (!isAArch64) break;
            if (!needBytes(8)) return false;
            writeVal(img, loc, S + signedAddend(8) - P, 8, state.endian);
            break;
          }
          case 261: {  // R_AARCH64_PREL32
            if (!isAArch64) break;
            if (!needBytes(4)) return false;
            uint64_t v = S + signedAddend(4) - P;
            if (!fitsS32(v)) {
              error = obj.label + ": relocation truncated to fit: R_AARCH64_PREL32";
              return false;
            }
            writeVal(img, loc, uint64_t(uint32_t(v)), 4, state.endian);
            break;
          }
          case 1: {  // RISC-V 32 / PPC ADDR32
            unsigned width = 4;
            if (isAArch64) {
              if (o.alloc) {
                error = obj.label + ": unsupported AArch64 relocation type 1";
                return false;
              }
              break;
            }
            if (isARM) {
              if (o.alloc) {
                error = obj.label + ": unsupported ARM relocation R_ARM_PC24";
                return false;
              }
              break;
            }
            if (!needBytes(width)) return false;
            uint64_t v = S + (uint64_t)A;
            if (!r.hasAddend) v = S + readVal(img, loc, width, state.endian);
            if (width == 4 && (v >> 32)) {
              error = obj.label + ": relocation truncated to fit: " +
                      r.typeName + " against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, v, width, state.endian);
            break;
          }
          case 2: {  // RISC-V 64 / ARM ABS32
            if (isAArch64 || isPPC) {
              if (o.alloc) {
                error = obj.label + ": unsupported relocation type 2";
                return false;
              }
              break;
            }
            unsigned width = state.arch == qbfd::Arch::RISCV64 ? 8 : 4;
            if (!needBytes(width)) return false;
            uint64_t v = S + (uint64_t)A;
            if (!r.hasAddend) v = S + readVal(img, loc, width, state.endian);
            if (width == 4 && (v >> 32)) {
              error = obj.label + ": relocation truncated to fit: " +
                      r.typeName + " against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, v, width, state.endian);
            break;
          }
          case 3: {  // ARM/RISC-V REL32 (RISC-V type 3 is RELATIVE)
            if (isRISCV && type == 3) {
              // R_RISCV_RELATIVE is for dynamic images and is not a valid
              // static ET_REL input.
              if (o.alloc) {
                error = obj.label + ": unsupported R_RISCV_RELATIVE in relocatable input";
                return false;
              }
              break;
            }
            if (!needBytes(4)) return false;
            uint64_t v = S + signedAddend(4) - P;
            if (!fitsS32(v)) {
              error = obj.label + ": relocation truncated to fit: " +
                      r.typeName + " against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, uint64_t(uint32_t(v)), 4, state.endian);
            break;
          }
          case 26: { // PPC/RISC-V 32_PCREL
            if (!needBytes(4)) return false;
            uint64_t v = S + signedAddend(4) - P;
            if (!fitsS32(v)) {
              error = obj.label + ": relocation truncated to fit: " +
                      r.typeName + " against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, uint64_t(uint32_t(v)), 4, state.endian);
            break;
          }
          case 38: { // PPC64 ADDR64
            if (!needBytes(8)) return false;
            uint64_t v = S + uint64_t(A);
            if (!r.hasAddend) v = S + readVal(img, loc, 8, state.endian);
            writeVal(img, loc, v, 8, state.endian);
            break;
          }
          case 44: { // PPC64 REL64
            if (!needBytes(8)) return false;
            writeVal(img, loc, S + signedAddend(8) - P, 8, state.endian);
            break;
          }
          default:
            if (o.alloc) {
              error = obj.label + ": unsupported relocation " + r.typeName +
                      " (" + std::to_string(type) + ") in " + in.section.name +
                      " against `" + symName + "'";
              return false;
            }
            break;
        }
      } else {
        error = obj.label + ": relocation architecture is not supported by qobjld";
        return false;
      }
    }
  }
  }
  return true;
}

}  // namespace qld::detail
