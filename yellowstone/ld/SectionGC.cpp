// Reachability over native ELF input sections, using the linker's prevailing
// symbol definitions and the input object's own local/section symbols.
#include "internal.hpp"

namespace qld::detail {

bool validateLtoInput(std::span<const uint8_t> bytes, const std::string& label,
                      std::string& error) {
  if (qbfd::elf::ELFFile::sniff(bytes)) return true;
  if (bytes.size() >= 4 &&
      ((bytes[0] == 'B' && bytes[1] == 'C' && bytes[2] == 0xc0 && bytes[3] == 0xde) ||
       (bytes[0] == 0xde && bytes[1] == 0xc0 && bytes[2] == 0x17 && bytes[3] == 0x0b))) {
    error = label + ": LLVM bitcode is unsupported by --lto-script";
  } else {
    error = label + ": --lto-script requires ELF ET_REL inputs, not another format or an implicit script";
  }
  return false;
}

bool validateLtoObject(const qbfd::ObjectFile& object, const std::string& label,
                       std::string& error) {
  if (object.format() != qbfd::Format::ELF || object.fileType() != qbfd::FileType::Relocatable ||
      !object.segments().empty() || !object.dynamicSymbols().empty()) {
    error = label + ": --lto-script requires static ELF ET_REL inputs (no dynamic inputs)";
    return false;
  }
  using qbfd::Arch;
  const bool wide = object.is64Bit(), little = object.endian() == qbfd::Endian::Little;
  bool supported = false;
  switch (object.arch()) {
    case Arch::X86: supported = !wide && little; break;
    case Arch::X86_64: supported = wide && little; break;
    case Arch::AArch64: case Arch::PowerPC64: supported = wide; break;
    case Arch::ARM: case Arch::PowerPC: supported = !wide; break;
    case Arch::RISCV32: case Arch::RISCV64: supported = little; break;
    default: break;
  }
  if (!supported) {
    error = label + ": --lto-script does not support this ELF machine/class/byte order";
    return false;
  }
  for (const auto& section : object.sections()) {
    const auto& name = section.name;
    if (name.starts_with(".gnu.lto_") || name.starts_with(".gnu.offload_lto_") ||
        name.starts_with(".gnu.debuglto_") ||
        name == ".llvmbc" || name == ".llvmcmd" || name == ".llvm_bc" ||
        name.starts_with(".llvm.lto")) {
      error = label + ": embedded compiler bitcode is unsupported by --lto-script: " + name;
      return false;
    }
    if (section.nativeType == qbfd::elf::k::Dynamic || section.nativeType == qbfd::elf::k::Dynsym ||
        section.nativeType == 19 /* SHT_RELR */) {
      error = label + ": dynamic metadata is unsupported by --lto-script: " + name;
      return false;
    }
  }
  return true;
}

namespace {

// The bounded static relocation subset for which the native writer has a
// matching implementation. Do not allow GC to hide unsupported relocations.
unsigned relocationWidth(qbfd::Arch arch, uint32_t type) {
  using qbfd::Arch;
  switch (arch) {
    case Arch::X86_64:
      if (type == 1) return 8;
      if (type == 2 || type == 4 || type == 10 || type == 11) return 4;
      break;
    case Arch::X86:
      if (type == 1 || type == 2) return 4;
      break;
    case Arch::AArch64:
      if (type == 257 || type == 260) return 8;
      if (type == 258 || type == 261) return 4;
      break;
    case Arch::RISCV64:
      if (type == 2) return 8;
      [[fallthrough]];
    case Arch::RISCV32:
      if (type == 1) return 4;
      break;
    case Arch::ARM:
      if (type == 2 || type == 3) return 4;
      break;
    case Arch::PowerPC64:
      if (type == 38 || type == 44) return 8;
      [[fallthrough]];
    case Arch::PowerPC:
      if (type == 1 || type == 26) return 4;
      break;
    default: break;
  }
  return 0;
}

bool candidate(const qbfd::Section& section) {
  // Only known, ordinary allocatable PROGBITS/NOBITS families can be removed.
  // Keep opaque metadata, startup tables, unwind data, SHF_GNU_RETAIN and
  // other unfamiliar flags. Relocations from them still root their targets.
  constexpr uint64_t ordinaryFlags = 0x37; // SHF_WRITE|ALLOC|EXECINSTR|MERGE|STRINGS
  if (!(section.flags & qbfd::sec::Alloc) ||
      (section.nativeType != qbfd::elf::k::Progbits && section.nativeType != qbfd::elf::k::Nobits) ||
      (section.nativeFlags & ~ordinaryFlags))
    return false;
  for (std::string_view family : {".text", ".rodata", ".data", ".bss", ".sdata", ".sbss"}) {
    const std::string_view name = section.name;
    if (name == family ||
        (name.size() > family.size() && name.starts_with(family) && name[family.size()] == '.'))
      return true;
  }
  return false;
}

} // namespace

bool collectLtoSections(LinkState& state, const std::vector<LtoSectionUse>& uses,
                        const std::set<std::string>& scriptRoots, std::string& error) try {
  auto& sections = state.inputSections;
  constexpr size_t maxRelocations = 1024 * 1024;
  if (!uses.empty() && uses.size() != sections.size())
    throw std::runtime_error("internal error: layout/section graph mismatch");
  std::map<std::pair<size_t, uint32_t>, size_t> index;
  size_t relocations = 0;
  for (size_t i = 0; i < sections.size(); ++i) {
    auto& in = sections[i];
    in.kept = false;
    index.emplace(std::make_pair(in.objectIndex, in.sectionIndex), i);
    if (in.relocs.size() > maxRelocations - relocations)
      throw std::runtime_error("relocation count limit exceeded");
    relocations += in.relocs.size();
    const auto& object = state.objects[in.objectIndex];
    constexpr uint64_t indexDependencies = 0x40 | 0x80 | 0x200;
    if (in.section.nativeFlags & indexDependencies) {
      // SHF_INFO_LINK, SHF_LINK_ORDER, SHF_GROUP can encode dependencies
      // not present in the relocation graph. Relocation tables were excluded.
      throw std::runtime_error(object.label + ": unsupported linked/grouped section " + in.section.name);
    }
    for (const auto& relocation : in.relocs) {
      unsigned width = relocationWidth(state.arch, relocation.nativeType);
      if (!width)
        throw std::runtime_error(object.label + ": unsupported relocation " +
                                 relocation.typeName + " in " + in.section.name);
      if (relocation.offset > in.contents.size() || width > in.contents.size() - relocation.offset)
        throw std::runtime_error(object.label + ": relocation offset outside input section " + in.section.name);
      if (relocation.dynamicSymbol || relocation.targetSection || relocation.scattered ||
          (relocation.symbol && *relocation.symbol >= object.object->symbols().size()))
        throw std::runtime_error(object.label + ": unsupported relocation target in " + in.section.name);
    }
  }

  auto sectionIndex = [&](size_t object, const qbfd::Symbol& symbol) -> std::optional<size_t> {
    if (symbol.section) {
      auto it = index.find({object, *symbol.section});
      if (it == index.end())
        throw std::runtime_error(state.objects[object].label +
                                 ": missing input section for relocation/root symbol `" + symbol.name + "'");
      if (symbol.value > sections[it->second].section.size)
        throw std::runtime_error(state.objects[object].label + ": definition outside its input section: " +
                                 symbol.name);
      return it->second;
    }
    return std::nullopt; // COMMON and absolute definitions occupy no input section.
  };
  auto targetSection = [&](const InputSection& in,
                           const qbfd::Relocation& relocation) -> std::optional<size_t> {
    if (!relocation.symbol) return std::nullopt;
    const auto& object = state.objects[in.objectIndex];
    const auto& symbol = object.object->symbols()[*relocation.symbol];
    if (usesGlobalRelocationSymbol(symbol)) {
      auto it = state.defs.find(symbol.name);
      if (it != state.defs.end())
        return sectionIndex(it->second.objectIndex, it->second.symbol);
      if ((symbol.flags & qbfd::sym::Undefined) && !state.options.allowUndefined)
        throw std::runtime_error(object.label + ": undefined reference to `" + symbol.name + "'");
    }
    return sectionIndex(in.objectIndex, symbol);
  };
  // Check references even in dead sections before pruning prevailing
  // definitions. The normal relocation writer still range-checks live writes.
  for (const auto& in : sections)
    for (const auto& relocation : in.relocs)
      targetSection(in, relocation);

  auto use = [&](size_t i) { return uses.empty() ? LtoSectionUse::Available : uses[i]; };
  std::vector<size_t> worklist;
  auto mark = [&](std::optional<size_t> i) {
    if (!i || sections[*i].kept) return;
    auto& in = sections[*i];
    if (use(*i) == LtoSectionUse::Discard)
      throw std::runtime_error("required section explicitly discarded: " +
                               state.objects[in.objectIndex].label + "(" + in.section.name + ")");
    in.kept = true;
    worklist.push_back(*i);
  };

  const auto entry = state.options.entry.empty() ? "_start" : state.options.entry;
  const auto entryDef = state.defs.find(entry);
  if (entryDef == state.defs.end() || !entryDef->second.symbol.section)
    throw std::runtime_error("section GC requires an entry symbol defined in an input code section: " + entry);
  auto entrySection = *sectionIndex(entryDef->second.objectIndex, entryDef->second.symbol);
  const auto& code = sections[entrySection].section;
  if (!(code.flags & qbfd::sec::Alloc) || !(code.flags & qbfd::sec::Exec) ||
      (code.flags & qbfd::sec::Bss) || entryDef->second.symbol.value >= code.size)
    throw std::runtime_error("entry is not inside an allocatable input code section: " + entry);
  mark(entrySection);

  for (const auto& name : state.ltoPlan.keepSymbols) {
    auto it = state.defs.find(name);
    if (it == state.defs.end() || !it->second.symbol.section)
      throw std::runtime_error("unknown keep symbol or not defined in an input section: " + name);
    mark(sectionIndex(it->second.objectIndex, it->second.symbol));
  }
  for (const auto& name : scriptRoots) {
    auto it = state.defs.find(name);
    if (it != state.defs.end())
      mark(sectionIndex(it->second.objectIndex, it->second.symbol));
  }
  for (size_t i = 0; i < sections.size(); ++i) {
    if (use(i) == LtoSectionUse::Keep ||
        (use(i) != LtoSectionUse::Discard && !candidate(sections[i].section)))
      mark(i);
  }

  for (size_t next = 0; next < worklist.size(); ++next) {
    const auto& in = sections[worklist[next]];
    for (const auto& relocation : in.relocs) mark(targetSection(in, relocation));
  }

  // Do not let layout fabricate addresses for removed definitions. Never
  // change the prevailing definition of a referenced weak/global symbol.
  std::erase_if(state.defs, [&](const auto& item) {
    const auto& def = item.second;
    if (!def.symbol.section || def.common) return false;
    auto it = index.find({def.objectIndex, *def.symbol.section});
    return it == index.end() || !sections[it->second].kept;
  });
  return true;
} catch (const std::exception& e) {
  error = "--lto-script section GC: " + std::string(e.what());
  return false;
}

} // namespace qld::detail
