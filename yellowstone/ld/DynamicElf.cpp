// DynamicElf.cpp - bounded native ELF64/x86-64 dynamic image emitter.
//
// This is intentionally an ELF implementation, not a relabelled static image:
// it emits dynamic symbols/hashes, GOT/PLT, runtime relocations, DT_NEEDED,
// PT_DYNAMIC and (for executables) PT_INTERP.  Unsupported ABI mechanisms are
// diagnosed before an output image is published.
#include "internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>

namespace qld::detail {
namespace {

constexpr uint64_t pageSize = 0x1000;
constexpr uint64_t imageLimit = UINT64_C(512) * 1024 * 1024;

uint64_t checkedAdd(uint64_t a, uint64_t b) {
  if (a > imageLimit || b > imageLimit || b > imageLimit - a)
    throw std::runtime_error("native dynamic ELF exceeds 512 MiB size limit");
  return a + b;
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
  if (!alignment || (alignment & (alignment - 1)) || alignment > 65536)
    throw std::runtime_error("unsupported native dynamic ELF section alignment");
  return checkedAdd(value, alignment - 1) & ~(alignment - 1);
}

struct Writer {
  std::vector<uint8_t>& bytes;

  void put(uint64_t offset, uint64_t value, unsigned width) {
    if (offset > bytes.size() || width > bytes.size() - offset)
      throw std::runtime_error("native dynamic ELF write outside output");
    for (unsigned i = 0; i < width; ++i)
      bytes[size_t(offset + i)] = uint8_t(value >> (i * 8));
  }

  void copy(uint64_t offset, const std::vector<uint8_t>& source) {
    if (offset > bytes.size() || source.size() > bytes.size() - offset)
      throw std::runtime_error("native dynamic ELF copy outside output");
    std::copy(source.begin(), source.end(),
              bytes.begin() + ptrdiff_t(offset));
  }
};

void put(std::vector<uint8_t>& bytes, uint64_t offset, uint64_t value,
         unsigned width) {
  if (offset > bytes.size() || width > bytes.size() - offset)
    throw std::runtime_error("native dynamic ELF section write outside range");
  for (unsigned i = 0; i < width; ++i)
    bytes[size_t(offset + i)] = uint8_t(value >> (i * 8));
}

uint32_t elfHash(const std::string& name) {
  uint32_t hash = 0;
  for (unsigned char c : name) {
    hash = (hash << 4) + c;
    uint32_t high = hash & 0xf0000000u;
    if (high) hash ^= high >> 24;
    hash &= ~high;
  }
  return hash;
}

uint32_t gnuHash(const std::string& name) {
  uint32_t hash = 5381;
  for (unsigned char c : name) hash = hash * 33 + c;
  return hash;
}

uint8_t symbolType(qbfd::SymbolKind kind) {
  switch (kind) {
    case qbfd::SymbolKind::Object: return 1;
    case qbfd::SymbolKind::Function: return 2;
    case qbfd::SymbolKind::Tls: return 6;
    case qbfd::SymbolKind::Indirect: return 10;
    default: return 0;
  }
}

bool globalSymbol(const qbfd::Symbol& symbol) {
  return symbol.binding == qbfd::SymbolBinding::Global ||
         symbol.binding == qbfd::SymbolBinding::Weak ||
         symbol.binding == qbfd::SymbolBinding::Unique;
}

struct SymbolRef {
  const qbfd::Symbol* source = nullptr;
  const Def* definition = nullptr;
  std::string name;
  bool defined = false;
  bool absolute = false;
  bool local = false;
  bool hidden = false;
  bool protectedVisibility = false;
  bool weak = false;
};

SymbolRef resolveRef(const LinkState& state, size_t object,
                     const qbfd::Relocation& relocation) {
  SymbolRef result;
  if (!relocation.symbol) {
    result.defined = true;
    result.absolute = true;
    return result;
  }
  const auto& symbols = state.objects.at(object).object->symbols();
  if (*relocation.symbol >= symbols.size())
    throw std::runtime_error("dynamic relocation refers to a bad symbol");
  result.source = &symbols[*relocation.symbol];
  result.name = result.source->name;
  result.local = !globalSymbol(*result.source);
  result.hidden = (result.source->flags & qbfd::sym::Hidden) != 0;
  result.protectedVisibility =
      (result.source->flags & qbfd::sym::Protected) != 0;
  result.weak = result.source->binding == qbfd::SymbolBinding::Weak;
  if (result.source->flags & qbfd::sym::Undefined) {
    auto found = state.defs.find(result.name);
    if (found != state.defs.end()) {
      result.definition = &found->second;
      result.defined = true;
      result.absolute =
          (result.definition->symbol.flags & qbfd::sym::Absolute) != 0;
      result.hidden =
          (result.definition->symbol.flags & qbfd::sym::Hidden) != 0;
      result.protectedVisibility =
          (result.definition->symbol.flags & qbfd::sym::Protected) != 0;
      result.weak = result.definition->weak;
      result.local = false;
    }
  } else {
    result.defined = true;
    result.absolute =
        (result.source->flags & qbfd::sym::Absolute) != 0;
  }
  return result;
}

std::string symbolKey(const SymbolRef& symbol, size_t object,
                      uint32_t symbolIndex) {
  if (!symbol.local && !symbol.name.empty()) return "G:" + symbol.name;
  return "L:" + std::to_string(object) + ":" +
         std::to_string(symbolIndex);
}

bool preemptible(const SymbolRef& symbol) {
  return !symbol.local && !symbol.hidden &&
         !symbol.protectedVisibility && !symbol.name.empty() &&
         !symbol.absolute;
}

enum class PlanKind : uint8_t {
  Ignore,
  Absolute,
  Pc32,
  Pc64,
  GotPc32,
  PltPc32,
  Dynamic64,
};

struct RelocationPlan {
  size_t input = 0;
  size_t relocation = 0;
  PlanKind kind = PlanKind::Ignore;
  std::string key;
};

struct GotEntry {
  std::string key;
  SymbolRef symbol;
  size_t object = 0;
  uint32_t symbolIndex = 0;
  uint64_t offset = 0;
};

struct PltEntry {
  std::string name;
  SymbolRef symbol;
  uint64_t index = 0;
};

struct DynamicSymbol {
  std::string name;
  uint64_t value = 0;
  uint64_t size = 0;
  uint8_t info = 0;
  uint8_t other = 0;
  uint16_t section = 0;
};

struct DynamicRelocation {
  uint64_t offset = 0;
  uint32_t symbol = 0;
  uint32_t type = 0;
  int64_t addend = 0;
};

struct Tag {
  uint64_t tag = 0;
  uint64_t value = 0;
  std::string addressOf;
};

struct Segment {
  uint32_t type = 0;
  uint32_t flags = 0;
  uint64_t offset = 0;
  uint64_t address = 0;
  uint64_t fileSize = 0;
  uint64_t memorySize = 0;
  uint64_t alignment = pageSize;
};

class DynamicImage {
 public:
  explicit DynamicImage(LinkState& state) : state_(state) {}

  void emit(std::vector<uint8_t>& output) {
    validate();
    captureCommons();
    planRelocations();
    collectNeeded();
    buildSymbolsAndStrings();
    buildTags();
    addSyntheticSections();
    layout();
    resolveAddresses();
    encodeSections();
    emitImage(output);
    buildMap();
  }

 private:
  LinkState& state_;
  bool shared_ = false;
  bool pie_ = false;
  bool executable_ = false;
  uint64_t base_ = 0;
  std::vector<RelocationPlan> plans_;
  std::map<std::string, GotEntry> got_;
  std::map<std::string, PltEntry> plt_;
  std::vector<DynamicSymbol> symbols_;
  std::map<std::string, uint32_t> symbolIndex_;
  uint32_t gnuSymbolOffset_ = 1;
  std::vector<std::string> needed_;
  std::map<std::string, uint32_t> strings_;
  std::vector<uint8_t> dynstr_{0};
  std::vector<Tag> tags_;
  std::vector<DynamicRelocation> relaDynamic_;
  std::vector<DynamicRelocation> relaPlt_;
  std::map<std::string, uint64_t> commonOffsets_;
  std::vector<std::vector<uint8_t>> staged_;
  std::vector<Segment> segments_;
  uint64_t relroAddress_ = 0;
  uint64_t relroSize_ = 0;

  OutputSection* section(const std::string& name) {
    for (auto& output : state_.outSections)
      if (output.name == name) return &output;
    return nullptr;
  }

  const OutputSection* section(const std::string& name) const {
    for (const auto& output : state_.outSections)
      if (output.name == name) return &output;
    return nullptr;
  }

  size_t outputForInput(size_t input) const {
    for (size_t i = 0; i < state_.outSections.size(); ++i)
      if (std::find(state_.outSections[i].inputs.begin(),
                    state_.outSections[i].inputs.end(),
                    input) != state_.outSections[i].inputs.end())
        return i;
    throw std::runtime_error("dynamic relocation target section was removed");
  }

  void validate() {
    shared_ = state_.options.mode == Mode::Shared;
    pie_ = state_.options.pie;
    executable_ = !shared_;
    if (!state_.wide || state_.endian != qbfd::Endian::Little ||
        state_.arch != qbfd::Arch::X86_64)
      throw std::runtime_error(
          "native dynamic ELF currently supports little-endian x86-64 only");
    if (state_.options.gcSections)
      throw std::runtime_error(
          "native dynamic ELF section GC is not yet supported");
    if (state_.outSections.empty())
      throw std::runtime_error("native dynamic ELF has no relocatable input sections");
    base_ = shared_ || pie_ ? 0 :
        (state_.options.hasBase ? state_.options.base : UINT64_C(0x400000));
    if (base_ > UINT64_MAX - imageLimit || base_ % pageSize)
      throw std::runtime_error("invalid native dynamic ELF image base");
    static const std::set<std::string> reserved = {
        ".interp", ".hash", ".gnu.hash", ".dynsym", ".dynstr",
        ".rela.dyn", ".rela.plt", ".plt", ".got", ".got.plt",
        ".dynamic"};
    for (const auto& output : state_.outSections)
      if (reserved.count(output.name) && output.size)
        throw std::runtime_error(
            "input defines linker-owned dynamic section " + output.name);
  }

  void captureCommons() {
    const auto* bss = section(".bss");
    if (!bss) return;
    for (const auto& [name, definition] : state_.defs)
      if (definition.common) {
        auto found = state_.symAddr.find(name);
        if (found != state_.symAddr.end() && found->second >= bss->vma)
          commonOffsets_[name] = found->second - bss->vma;
      }
  }

  bool allocatedTarget(size_t input) const {
    return state_.outSections[outputForInput(input)].alloc;
  }

  bool writableTarget(size_t input) const {
    return state_.outSections[outputForInput(input)].writable;
  }

  void planRelocations() {
    for (size_t inputIndex = 0;
         inputIndex < state_.inputSections.size(); ++inputIndex) {
      const auto& input = state_.inputSections[inputIndex];
      if (!input.kept) continue;
      for (size_t relocationIndex = 0;
           relocationIndex < input.relocs.size(); ++relocationIndex) {
        const auto& relocation = input.relocs[relocationIndex];
        RelocationPlan plan;
        plan.input = inputIndex;
        plan.relocation = relocationIndex;
        SymbolRef symbol =
            resolveRef(state_, input.objectIndex, relocation);
        uint32_t symbolIndex = relocation.symbol.value_or(0);
        plan.key = symbolKey(symbol, input.objectIndex, symbolIndex);
        bool allocated = allocatedTarget(inputIndex);
        switch (relocation.nativeType) {
          case 0:
            plan.kind = PlanKind::Ignore;
            break;
          case 1:  // R_X86_64_64
            if (!allocated || symbol.absolute)
              plan.kind = PlanKind::Absolute;
            else {
              if (!writableTarget(inputIndex))
                throw std::runtime_error(
                    state_.objects[input.objectIndex].label +
                    ": dynamic absolute relocation in read-only section " +
                    input.section.name + "; recompile as PIC");
              plan.kind = PlanKind::Dynamic64;
            }
            break;
          case 2:  // R_X86_64_PC32
            if (!symbol.defined ||
                ((shared_ || pie_) && allocated && preemptible(symbol)))
              throw std::runtime_error(
                  state_.objects[input.objectIndex].label +
                  ": R_X86_64_PC32 against dynamic symbol `" + symbol.name +
                  "'; recompile as PIC");
            plan.kind = PlanKind::Pc32;
            break;
          case 4:  // R_X86_64_PLT32
            if (preemptible(symbol)) {
              if (!symbol.defined && state_.options.noUndefined &&
                  !state_.dynamicDefs.count(symbol.name))
                throw std::runtime_error("undefined reference to `" +
                                         symbol.name + "'");
              plan.kind = PlanKind::PltPc32;
              plt_.try_emplace(symbol.name,
                               PltEntry{symbol.name, symbol, 0});
            } else {
              if (!symbol.defined)
                throw std::runtime_error("undefined local/hidden symbol `" +
                                         symbol.name + "'");
              plan.kind = PlanKind::Pc32;
            }
            break;
          case 9:   // R_X86_64_GOTPCREL
          case 41:  // R_X86_64_GOTPCRELX
          case 42:  // R_X86_64_REX_GOTPCRELX
            plan.kind = PlanKind::GotPc32;
            got_.try_emplace(
                plan.key,
                GotEntry{plan.key, symbol, input.objectIndex, symbolIndex, 0});
            break;
          case 10:  // R_X86_64_32
          case 11:  // R_X86_64_32S
            if ((shared_ || pie_) && allocated && !symbol.absolute)
              throw std::runtime_error(
                  state_.objects[input.objectIndex].label +
                  ": non-position-independent " + relocation.typeName +
                  " in dynamic image; recompile as PIC/PIE");
            if (!symbol.defined)
              throw std::runtime_error("unsupported dynamic 32-bit relocation "
                                       "against `" + symbol.name + "'");
            plan.kind = PlanKind::Absolute;
            break;
          case 24:  // R_X86_64_PC64
            if (!symbol.defined)
              throw std::runtime_error("R_X86_64_PC64 against dynamic symbol `" +
                                       symbol.name + "'");
            plan.kind = PlanKind::Pc64;
            break;
          default:
            if (allocated)
              throw std::runtime_error(
                  state_.objects[input.objectIndex].label +
                  ": unsupported native dynamic ELF relocation " +
                  relocation.typeName + " (" +
                  std::to_string(relocation.nativeType) + ") in " +
                  input.section.name);
            plan.kind = PlanKind::Ignore;
            break;
        }
        plans_.push_back(std::move(plan));
      }
    }
    uint64_t index = 0;
    for (auto& [name, entry] : plt_) {
      (void)name;
      entry.index = index++;
    }
    uint64_t offset = 0;
    for (auto& [key, entry] : got_) {
      (void)key;
      entry.offset = offset;
      offset = checkedAdd(offset, 8);
    }
  }

  bool dynamicObjectUsed(size_t object) const {
    for (const auto& plan : plans_) {
      if (plan.kind != PlanKind::GotPc32 &&
          plan.kind != PlanKind::PltPc32 &&
          plan.kind != PlanKind::Dynamic64)
        continue;
      const auto& input = state_.inputSections[plan.input];
      const auto& relocation = input.relocs[plan.relocation];
      SymbolRef symbol =
          resolveRef(state_, input.objectIndex, relocation);
      auto provider = state_.dynamicDefs.find(symbol.name);
      if (!symbol.defined && provider != state_.dynamicDefs.end() &&
          provider->second == object)
        return true;
    }
    return false;
  }

  void collectNeeded() {
    std::set<std::string> seen;
    for (size_t object : state_.dynamicObjects) {
      if (state_.options.asNeeded && !dynamicObjectUsed(object)) continue;
      const auto& input = state_.objects[object];
      std::string name = input.object->soname().value_or(
          std::filesystem::path(input.label).filename().string());
      if (name.empty())
        throw std::runtime_error("dynamic input has no usable runtime name");
      if (seen.insert(name).second) needed_.push_back(std::move(name));
    }
  }

  qbfd::Symbol symbolMetadata(const std::string& name) const {
    auto definition = state_.defs.find(name);
    if (definition != state_.defs.end()) return definition->second.symbol;
    for (const auto& input : state_.objects) {
      if (!input.object) continue;
      for (const auto& symbol : input.object->symbols())
        if (symbol.name == name) return symbol;
      for (const auto& symbol : input.object->dynamicSymbols())
        if (symbol.name == name) return symbol;
    }
    qbfd::Symbol symbol;
    symbol.name = name;
    symbol.binding = qbfd::SymbolBinding::Global;
    return symbol;
  }

  uint32_t intern(const std::string& text) {
    if (text.empty()) return 0;
    auto found = strings_.find(text);
    if (found != strings_.end()) return found->second;
    if (dynstr_.size() > UINT32_MAX - text.size() - 1)
      throw std::runtime_error("native dynamic ELF string table is too large");
    uint32_t offset = uint32_t(dynstr_.size());
    dynstr_.insert(dynstr_.end(), text.begin(), text.end());
    dynstr_.push_back(0);
    strings_[text] = offset;
    return offset;
  }

  void buildSymbolsAndStrings() {
    std::set<std::string> names;
    if (shared_ || state_.options.exportDynamic)
      for (const auto& [name, definition] : state_.defs)
        if (!(definition.symbol.flags & qbfd::sym::Hidden))
          names.insert(name);
    for (const auto& [key, entry] : got_) {
      (void)key;
      if (!entry.symbol.local && !entry.symbol.hidden &&
          !entry.symbol.name.empty())
        names.insert(entry.symbol.name);
    }
    for (const auto& [name, entry] : plt_) {
      (void)entry;
      names.insert(name);
    }
    for (const auto& plan : plans_) {
      if (plan.kind != PlanKind::Dynamic64) continue;
      const auto& input = state_.inputSections[plan.input];
      SymbolRef symbol = resolveRef(
          state_, input.objectIndex, input.relocs[plan.relocation]);
      if (preemptible(symbol)) names.insert(symbol.name);
    }

    std::vector<std::string> ordered(names.begin(), names.end());
    if (ordered.size() >= UINT32_MAX)
      throw std::runtime_error("too many native dynamic ELF symbols");
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&](const std::string& left,
                         const std::string& right) {
      bool leftDefined = state_.defs.count(left) != 0;
      bool rightDefined = state_.defs.count(right) != 0;
      if (leftDefined != rightDefined) return !leftDefined;
      return left < right;
    });
    symbols_.push_back({});
    bool foundDefinition = false;
    for (const auto& name : ordered) {
      bool defined = state_.defs.count(name) != 0;
      if (defined && !foundDefinition) {
        gnuSymbolOffset_ = uint32_t(symbols_.size());
        foundDefinition = true;
      }
      qbfd::Symbol source = symbolMetadata(name);
      DynamicSymbol symbol;
      symbol.name = name;
      symbol.size = source.size;
      uint8_t binding =
          source.binding == qbfd::SymbolBinding::Weak ? 2 : 1;
      symbol.info = uint8_t((binding << 4) | symbolType(source.kind));
      if (source.flags & qbfd::sym::Protected) symbol.other = 3;
      symbols_.push_back(std::move(symbol));
      symbolIndex_[name] = uint32_t(symbols_.size() - 1);
      intern(name);
    }
    if (!foundDefinition) gnuSymbolOffset_ = uint32_t(symbols_.size());
    for (const auto& name : needed_) intern(name);
    intern(state_.options.soname);
    intern(state_.options.runpath);
  }

  void addTag(uint64_t tag, uint64_t value = 0,
              std::string addressOf = {}) {
    tags_.push_back({tag, value, std::move(addressOf)});
  }

  bool hasUserSection(const std::string& name) const {
    const auto* found = section(name);
    return found && found->size;
  }

  void buildTags() {
    for (const auto& name : needed_) addTag(1, strings_.at(name));  // DT_NEEDED
    if (!state_.options.soname.empty())
      addTag(14, strings_.at(state_.options.soname));
    if (!state_.options.runpath.empty())
      addTag(state_.options.newDtags ? 29 : 15,
             strings_.at(state_.options.runpath));
    if (state_.options.hashStyle == LinkOptions::HashStyle::SysV ||
        state_.options.hashStyle == LinkOptions::HashStyle::Both)
      addTag(4, 0, ".hash");
    if (state_.options.hashStyle == LinkOptions::HashStyle::GNU ||
        state_.options.hashStyle == LinkOptions::HashStyle::Both)
      addTag(0x6ffffef5, 0, ".gnu.hash");
    addTag(5, 0, ".dynstr");
    addTag(6, 0, ".dynsym");
    addTag(10, dynstr_.size());
    addTag(11, 24);
    size_t dynamic64 = 0;
    for (const auto& plan : plans_)
      if (plan.kind == PlanKind::Dynamic64) ++dynamic64;
    if (!got_.empty() || dynamic64) {
      addTag(7, 0, ".rela.dyn");
      addTag(8, (got_.size() + dynamic64) * 24);
      addTag(9, 24);
    }
    if (!plt_.empty()) {
      addTag(3, 0, ".got.plt");
      addTag(2, plt_.size() * 24);
      addTag(20, 7);  // DT_PLTREL == DT_RELA
      addTag(23, 0, ".rela.plt");
    }
    if (hasUserSection(".init")) addTag(12, 0, ".init");
    if (hasUserSection(".fini")) addTag(13, 0, ".fini");
    if (hasUserSection(".init_array")) {
      addTag(25, 0, ".init_array");
      addTag(27, section(".init_array")->size);
    }
    if (hasUserSection(".fini_array")) {
      addTag(26, 0, ".fini_array");
      addTag(28, section(".fini_array")->size);
    }
    if (hasUserSection(".preinit_array") && executable_) {
      addTag(32, 0, ".preinit_array");
      addTag(33, section(".preinit_array")->size);
    }
    if (executable_) addTag(21);  // DT_DEBUG, populated by the loader
    if (state_.options.bindNow) addTag(30, 8);  // DF_BIND_NOW
    if (pie_ || state_.options.bindNow)
      addTag(0x6ffffffb,
             (pie_ ? UINT64_C(0x08000000) : 0) |
             (state_.options.bindNow ? 1 : 0));
    addTag(0);
  }

  OutputSection synthetic(const std::string& name, uint32_t flags,
                          uint64_t size, uint64_t alignment) {
    OutputSection output;
    output.name = name;
    output.flags = flags;
    output.alignment = alignment;
    output.size = output.fileSize = size;
    output.alloc = true;
    output.executable =
        (flags & (qbfd::sec::Code | qbfd::sec::Exec)) != 0;
    output.writable = (flags & qbfd::sec::Writable) != 0;
    output.scriptData.resize(size_t(size), 0);
    return output;
  }

  void addSyntheticSections() {
    const uint32_t read = qbfd::sec::Alloc | qbfd::sec::Load |
                          qbfd::sec::ReadOnly | qbfd::sec::Data;
    const uint32_t write = qbfd::sec::Alloc | qbfd::sec::Load |
                           qbfd::sec::Writable | qbfd::sec::Data;
    const uint32_t execute = qbfd::sec::Alloc | qbfd::sec::Load |
                             qbfd::sec::ReadOnly | qbfd::sec::Code |
                             qbfd::sec::Exec;
    std::vector<OutputSection> old = std::move(state_.outSections);
    std::vector<OutputSection> ro, rx, rw, bss, other;
    for (auto& output : old) {
      if (!output.alloc) other.push_back(std::move(output));
      else if (output.bss) bss.push_back(std::move(output));
      else if (output.writable) rw.push_back(std::move(output));
      else if (output.executable) rx.push_back(std::move(output));
      else ro.push_back(std::move(output));
    }

    std::vector<OutputSection> ordered;
    std::string interpreter = state_.options.dynamicLinker.empty()
        ? "/lib64/ld-linux-x86-64.so.2"
        : state_.options.dynamicLinker;
    if (executable_)
      ordered.push_back(synthetic(".interp", read,
                                  interpreter.size() + 1, 1));
    if (state_.options.hashStyle == LinkOptions::HashStyle::SysV ||
        state_.options.hashStyle == LinkOptions::HashStyle::Both)
      ordered.push_back(synthetic(".hash", read,
                                  (2 + 1 + symbols_.size()) * 4, 8));
    if (state_.options.hashStyle == LinkOptions::HashStyle::GNU ||
        state_.options.hashStyle == LinkOptions::HashStyle::Both) {
      uint64_t chains = symbols_.size() - gnuSymbolOffset_;
      ordered.push_back(synthetic(".gnu.hash", read,
                                  16 + 8 + 4 + chains * 4, 8));
    }
    ordered.push_back(
        synthetic(".dynsym", read, symbols_.size() * 24, 8));
    ordered.push_back(synthetic(".dynstr", read, dynstr_.size(), 1));
    size_t dynamic64 = 0;
    for (const auto& plan : plans_)
      if (plan.kind == PlanKind::Dynamic64) ++dynamic64;
    if (!got_.empty() || dynamic64)
      ordered.push_back(synthetic(
          ".rela.dyn", read, (got_.size() + dynamic64) * 24, 8));
    if (!plt_.empty())
      ordered.push_back(
          synthetic(".rela.plt", read, plt_.size() * 24, 8));
    for (auto& output : ro) ordered.push_back(std::move(output));
    if (!plt_.empty())
      ordered.push_back(
          synthetic(".plt", execute, (plt_.size() + 1) * 16, 16));
    for (auto& output : rx) ordered.push_back(std::move(output));
    ordered.push_back(
        synthetic(".dynamic", write, tags_.size() * 16, 8));
    if (!got_.empty())
      ordered.push_back(synthetic(".got", write, got_.size() * 8, 8));
    if (!plt_.empty())
      ordered.push_back(
          synthetic(".got.plt", write, (plt_.size() + 3) * 8, 8));
    for (auto& output : rw) ordered.push_back(std::move(output));
    for (auto& output : bss) ordered.push_back(std::move(output));
    for (auto& output : other) ordered.push_back(std::move(output));
    state_.outSections = std::move(ordered);
    if (state_.outSections.size() > 0xfffd)
      throw std::runtime_error("too many native dynamic ELF sections");
    for (size_t i = 0; i < state_.outSections.size(); ++i)
      state_.outSections[i].index = uint32_t(i + 1);
  }

  void placeFileSections(const std::vector<size_t>& list,
                         uint64_t& cursor) {
    for (size_t index : list) {
      auto& output = state_.outSections[index];
      cursor = alignUp(cursor, std::max<uint64_t>(output.alignment, 1));
      output.fileOffset = cursor;
      output.vma = checkedAdd(base_, cursor);
      cursor = checkedAdd(cursor, output.fileSize);
    }
  }

  void layout() {
    std::vector<size_t> ro, rx, relro, rw, bss, other;
    for (size_t i = 0; i < state_.outSections.size(); ++i) {
      const auto& output = state_.outSections[i];
      if (!output.alloc) other.push_back(i);
      else if (output.bss) bss.push_back(i);
      else if (output.name == ".dynamic" || output.name == ".got")
        relro.push_back(i);
      else if (output.writable) rw.push_back(i);
      else if (output.executable) rx.push_back(i);
      else ro.push_back(i);
    }
    uint64_t loadCount = 2 + (rx.empty() ? 0 : 1);
    uint64_t phnum = 1 + loadCount + 2 + (executable_ ? 1 : 0) +
                     (state_.options.relro ? 1 : 0);
    uint64_t headerEnd = checkedAdd(64, phnum * 56);
    uint64_t cursor = headerEnd;
    placeFileSections(ro, cursor);
    uint64_t roEnd = cursor;
    segments_.push_back({1, 4, 0, base_, roEnd, roEnd, pageSize});

    if (!rx.empty()) {
      cursor = alignUp(cursor, pageSize);
      uint64_t start = cursor;
      placeFileSections(rx, cursor);
      segments_.push_back(
          {1, 5, start, checkedAdd(base_, start),
           cursor - start, cursor - start, pageSize});
    }

    cursor = alignUp(cursor, pageSize);
    uint64_t rwStart = cursor;
    placeFileSections(relro, cursor);
    if (state_.options.relro && !relro.empty()) {
      relroAddress_ = checkedAdd(base_, rwStart);
      uint64_t end = alignUp(cursor, pageSize);
      relroSize_ = end - rwStart;
      cursor = end;
    }
    placeFileSections(rw, cursor);
    uint64_t fileEnd = cursor;
    uint64_t memory = cursor;
    for (size_t index : bss) {
      auto& output = state_.outSections[index];
      memory = alignUp(memory, std::max<uint64_t>(output.alignment, 1));
      output.fileOffset = fileEnd;
      output.vma = checkedAdd(base_, memory);
      output.fileSize = 0;
      memory = checkedAdd(memory, output.size);
    }
    segments_.push_back(
        {1, 6, rwStart, checkedAdd(base_, rwStart),
         fileEnd - rwStart, memory - rwStart, pageSize});

    cursor = fileEnd;
    for (size_t index : other) {
      auto& output = state_.outSections[index];
      cursor = alignUp(cursor, std::max<uint64_t>(output.alignment, 1));
      output.fileOffset = cursor;
      output.vma = 0;
      cursor = checkedAdd(cursor, output.fileSize);
    }
  }

  uint64_t localAddress(size_t object,
                        const qbfd::Symbol& symbol) const {
    if (symbol.flags & qbfd::sym::Absolute) return symbol.value;
    if (globalSymbol(symbol) && !symbol.name.empty()) {
      auto found = state_.symAddr.find(symbol.name);
      if (found != state_.symAddr.end()) return found->second;
    }
    if (!symbol.section)
      throw std::runtime_error("defined dynamic-link symbol has no section: " +
                               symbol.name);
    for (const auto& output : state_.outSections)
      for (size_t inputIndex : output.inputs) {
        const auto& input = state_.inputSections[inputIndex];
        if (input.objectIndex == object &&
            input.sectionIndex == *symbol.section)
          return checkedAdd(
              checkedAdd(output.vma, input.outOffset), symbol.value);
      }
    throw std::runtime_error("dynamic-link symbol belongs to a removed section: " +
                             symbol.name);
  }

  uint64_t symbolAddress(size_t object,
                         const qbfd::Relocation& relocation) const {
    if (!relocation.symbol) return 0;
    const auto& symbols = state_.objects.at(object).object->symbols();
    const auto& symbol = symbols.at(*relocation.symbol);
    if (symbol.flags & qbfd::sym::Undefined) {
      auto found = state_.symAddr.find(symbol.name);
      if (found == state_.symAddr.end()) return 0;
      return found->second;
    }
    return localAddress(object, symbol);
  }

  uint16_t symbolSection(const Def& definition) const {
    if (definition.symbol.flags & qbfd::sym::Absolute) return 0xfff1;
    if (definition.common) {
      const auto* bss = section(".bss");
      return bss ? uint16_t(bss->index) : 0;
    }
    if (!definition.symbol.section) return 0xfff1;
    for (const auto& output : state_.outSections)
      for (size_t inputIndex : output.inputs) {
        const auto& input = state_.inputSections[inputIndex];
        if (input.objectIndex == definition.objectIndex &&
            input.sectionIndex == *definition.symbol.section)
          return uint16_t(output.index);
      }
    return 0;
  }

  void resolveAddresses() {
    state_.symAddr.clear();
    for (const auto& [name, definition] : state_.defs) {
      if (definition.common) {
        const auto* bss = section(".bss");
        auto offset = commonOffsets_.find(name);
        if (!bss || offset == commonOffsets_.end())
          throw std::runtime_error("cannot place COMMON symbol " + name);
        state_.symAddr[name] = checkedAdd(bss->vma, offset->second);
      } else if (definition.symbol.flags & qbfd::sym::Absolute) {
        state_.symAddr[name] = definition.symbol.value;
      } else {
        state_.symAddr[name] =
            localAddress(definition.objectIndex, definition.symbol);
      }
    }
    for (auto& symbol : symbols_) {
      if (symbol.name.empty()) continue;
      auto found = state_.defs.find(symbol.name);
      if (found == state_.defs.end()) continue;
      symbol.value = state_.symAddr.at(symbol.name);
      symbol.size = found->second.common
          ? found->second.commonSize
          : found->second.symbol.size;
      symbol.section = symbolSection(found->second);
    }
    if (executable_) {
      auto found = state_.symAddr.find(state_.options.entry);
      if (found != state_.symAddr.end()) state_.entryAddr = found->second;
      else if (!state_.options.allowUndefined)
        throw std::runtime_error("undefined dynamic entry symbol: " +
                                 state_.options.entry);
    } else {
      state_.entryAddr = 0;
    }
  }

  std::vector<uint8_t>& data(const std::string& name) {
    for (size_t i = 0; i < state_.outSections.size(); ++i)
      if (state_.outSections[i].name == name) return staged_[i];
    throw std::runtime_error("missing synthetic dynamic section " + name);
  }

  const OutputSection& requiredSection(const std::string& name) const {
    const auto* found = section(name);
    if (!found)
      throw std::runtime_error("missing synthetic dynamic section " + name);
    return *found;
  }

  void stageInputSections() {
    staged_.resize(state_.outSections.size());
    for (size_t i = 0; i < state_.outSections.size(); ++i) {
      const auto& output = state_.outSections[i];
      if (output.bss) continue;
      auto bytes = output.scriptData;
      bytes.resize(size_t(output.fileSize), 0);
      for (size_t inputIndex : output.inputs) {
        const auto& input = state_.inputSections[inputIndex];
        if (input.outOffset > bytes.size() ||
            input.contents.size() > bytes.size() - input.outOffset)
          throw std::runtime_error("input section exceeds dynamic output section");
        std::copy(input.contents.begin(), input.contents.end(),
                  bytes.begin() + ptrdiff_t(input.outOffset));
      }
      staged_[i] = std::move(bytes);
    }
  }

  static int64_t signedAddend(const qbfd::Relocation& relocation,
                              const std::vector<uint8_t>& bytes,
                              uint64_t offset, unsigned width) {
    if (relocation.hasAddend) return relocation.addend;
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
      value |= uint64_t(bytes[size_t(offset + i)]) << (i * 8);
    if (width == 4) return int32_t(value);
    return int64_t(value);
  }

  uint32_t dynIndex(const SymbolRef& symbol) const {
    auto found = symbolIndex_.find(symbol.name);
    if (found == symbolIndex_.end())
      throw std::runtime_error("missing dynamic symbol " + symbol.name);
    return found->second;
  }

  void applyPlannedRelocations() {
    for (const auto& plan : plans_) {
      if (plan.kind == PlanKind::Ignore) continue;
      const auto& input = state_.inputSections[plan.input];
      const auto& relocation = input.relocs[plan.relocation];
      size_t outputIndex = outputForInput(plan.input);
      auto& bytes = staged_[outputIndex];
      uint64_t location = checkedAdd(input.outOffset, relocation.offset);
      unsigned width =
          plan.kind == PlanKind::Absolute ||
                  plan.kind == PlanKind::Pc64 ||
                  plan.kind == PlanKind::Dynamic64
              ? (relocation.nativeType == 10 ||
                         relocation.nativeType == 11
                     ? 4
                     : 8)
              : 4;
      if (location > bytes.size() || width > bytes.size() - location)
        throw std::runtime_error(
            state_.objects[input.objectIndex].label +
            ": dynamic relocation lies outside " + input.section.name);
      int64_t addend =
          signedAddend(relocation, bytes, location, width);
      uint64_t place = checkedAdd(
          state_.outSections[outputIndex].vma, location);
      uint64_t symbol = symbolAddress(input.objectIndex, relocation);
      SymbolRef reference =
          resolveRef(state_, input.objectIndex, relocation);
      switch (plan.kind) {
        case PlanKind::Absolute: {
          uint64_t value = symbol + uint64_t(addend);
          if (width == 4) {
            if (relocation.nativeType == 10 && value > UINT32_MAX)
              throw std::runtime_error("R_X86_64_32 truncated against `" +
                                       reference.name + "'");
            if (relocation.nativeType == 11 &&
                int64_t(value) != int64_t(int32_t(value)))
              throw std::runtime_error("R_X86_64_32S truncated against `" +
                                       reference.name + "'");
          }
          put(bytes, location, value, width);
          break;
        }
        case PlanKind::Pc32: {
          int64_t value = int64_t(symbol) + addend - int64_t(place);
          if (value != int64_t(int32_t(value)))
            throw std::runtime_error("R_X86_64_PC32 truncated against `" +
                                     reference.name + "'");
          put(bytes, location, uint32_t(value), 4);
          break;
        }
        case PlanKind::Pc64:
          put(bytes, location,
              uint64_t(int64_t(symbol) + addend - int64_t(place)), 8);
          break;
        case PlanKind::GotPc32: {
          const auto& got = requiredSection(".got");
          uint64_t target = checkedAdd(got.vma, got_.at(plan.key).offset);
          int64_t value = int64_t(target) + addend - int64_t(place);
          if (value != int64_t(int32_t(value)))
            throw std::runtime_error("GOTPCREL relocation truncated against `" +
                                     reference.name + "'");
          put(bytes, location, uint32_t(value), 4);
          break;
        }
        case PlanKind::PltPc32: {
          const auto& plt = requiredSection(".plt");
          uint64_t target = checkedAdd(
              plt.vma, 16 * (plt_.at(reference.name).index + 1));
          int64_t value = int64_t(target) + addend - int64_t(place);
          if (value != int64_t(int32_t(value)))
            throw std::runtime_error("PLT32 relocation truncated against `" +
                                     reference.name + "'");
          put(bytes, location, uint32_t(value), 4);
          break;
        }
        case PlanKind::Dynamic64:
          put(bytes, location, 0, 8);
          if (preemptible(reference))
            relaDynamic_.push_back(
                {place, dynIndex(reference), 1, addend});  // R_X86_64_64
          else
            relaDynamic_.push_back(
                {place, 0, 8, int64_t(symbol) + addend});  // RELATIVE
          break;
        case PlanKind::Ignore:
          break;
      }
    }
  }

  void buildGot() {
    if (got_.empty()) return;
    auto& bytes = data(".got");
    const auto& output = requiredSection(".got");
    for (const auto& [key, entry] : got_) {
      (void)key;
      uint64_t address = checkedAdd(output.vma, entry.offset);
      if (preemptible(entry.symbol)) {
        relaDynamic_.push_back(
            {address, dynIndex(entry.symbol), 6, 0});  // GLOB_DAT
      } else {
        uint64_t value = 0;
        if (entry.symbol.source)
          value = localAddress(entry.object, *entry.symbol.source);
        relaDynamic_.push_back(
            {address, 0, 8, int64_t(value)});  // RELATIVE
      }
      put(bytes, entry.offset, 0, 8);
    }
  }

  void buildPlt() {
    if (plt_.empty()) return;
    auto& code = data(".plt");
    auto& got = data(".got.plt");
    const auto& pltSection = requiredSection(".plt");
    const auto& gotSection = requiredSection(".got.plt");
    const auto& dynamic = requiredSection(".dynamic");
    put(got, 0, dynamic.vma, 8);
    auto displacement = [](uint64_t target, uint64_t next) {
      int64_t value = int64_t(target) - int64_t(next);
      if (value != int64_t(int32_t(value)))
        throw std::runtime_error("native dynamic PLT displacement overflow");
      return uint32_t(value);
    };
    code[0] = 0xff; code[1] = 0x35;
    put(code, 2, displacement(gotSection.vma + 8, pltSection.vma + 6), 4);
    code[6] = 0xff; code[7] = 0x25;
    put(code, 8, displacement(gotSection.vma + 16, pltSection.vma + 12), 4);
    code[12] = 0x0f; code[13] = 0x1f; code[14] = 0x40; code[15] = 0;
    for (const auto& [name, entry] : plt_) {
      uint64_t offset = 16 * (entry.index + 1);
      uint64_t address = pltSection.vma + offset;
      uint64_t slotOffset = 8 * (entry.index + 3);
      uint64_t slotAddress = gotSection.vma + slotOffset;
      code[size_t(offset)] = 0xff;
      code[size_t(offset + 1)] = 0x25;
      put(code, offset + 2,
          displacement(slotAddress, address + 6), 4);
      code[size_t(offset + 6)] = 0x68;
      put(code, offset + 7, entry.index, 4);
      code[size_t(offset + 11)] = 0xe9;
      put(code, offset + 12,
          displacement(pltSection.vma, address + 16), 4);
      put(got, slotOffset, address + 6, 8);
      relaPlt_.push_back(
          {slotAddress, symbolIndex_.at(name), 7, 0});  // JUMP_SLOT
    }
  }

  void encodeRelocations(const std::string& name,
                         const std::vector<DynamicRelocation>& relocations) {
    if (relocations.empty()) return;
    auto& bytes = data(name);
    if (bytes.size() != relocations.size() * 24)
      throw std::runtime_error("dynamic relocation count changed after layout");
    for (size_t i = 0; i < relocations.size(); ++i) {
      uint64_t offset = i * 24;
      put(bytes, offset, relocations[i].offset, 8);
      put(bytes, offset + 8,
          (uint64_t(relocations[i].symbol) << 32) |
              relocations[i].type,
          8);
      put(bytes, offset + 16, uint64_t(relocations[i].addend), 8);
    }
  }

  void encodeSymbols() {
    auto& bytes = data(".dynsym");
    for (size_t i = 0; i < symbols_.size(); ++i) {
      uint64_t offset = i * 24;
      put(bytes, offset,
          symbols_[i].name.empty() ? 0 : strings_.at(symbols_[i].name), 4);
      put(bytes, offset + 4, symbols_[i].info, 1);
      put(bytes, offset + 5, symbols_[i].other, 1);
      put(bytes, offset + 6, symbols_[i].section, 2);
      put(bytes, offset + 8, symbols_[i].value, 8);
      put(bytes, offset + 16, symbols_[i].size, 8);
    }
  }

  void encodeHashes() {
    if (section(".hash")) {
      auto& bytes = data(".hash");
      uint32_t buckets = 1;
      put(bytes, 0, buckets, 4);
      put(bytes, 4, symbols_.size(), 4);
      uint32_t previous = 0;
      for (uint32_t i = 1; i < symbols_.size(); ++i) {
        uint32_t bucket = elfHash(symbols_[i].name) % buckets;
        uint64_t bucketOffset = 8 + uint64_t(bucket) * 4;
        uint32_t first = 0;
        for (unsigned b = 0; b < 4; ++b)
          first |= uint32_t(bytes[size_t(bucketOffset + b)]) << (8 * b);
        if (!first) put(bytes, bucketOffset, i, 4);
        if (previous) put(bytes, 8 + (buckets + previous) * 4, i, 4);
        previous = i;
      }
    }
    if (section(".gnu.hash")) {
      auto& bytes = data(".gnu.hash");
      uint32_t count = uint32_t(symbols_.size());
      put(bytes, 0, 1, 4);  // nbuckets
      put(bytes, 4, gnuSymbolOffset_, 4);
      put(bytes, 8, 1, 4);  // bloom size
      put(bytes, 12, 6, 4); // bloom shift
      uint64_t bloom = 0;
      for (uint32_t i = gnuSymbolOffset_; i < count; ++i) {
        uint32_t hash = gnuHash(symbols_[i].name);
        bloom |= UINT64_C(1) << (hash % 64);
        bloom |= UINT64_C(1) << ((hash >> 6) % 64);
      }
      put(bytes, 16, bloom, 8);
      put(bytes, 24, count > gnuSymbolOffset_ ? gnuSymbolOffset_ : 0, 4);
      for (uint32_t i = gnuSymbolOffset_; i < count; ++i) {
        uint32_t hash = gnuHash(symbols_[i].name) & ~1u;
        if (i + 1 == count) hash |= 1;
        put(bytes, 28 + uint64_t(i - gnuSymbolOffset_) * 4, hash, 4);
      }
    }
  }

  void encodeDynamic() {
    auto& bytes = data(".dynamic");
    for (size_t i = 0; i < tags_.size(); ++i) {
      uint64_t value = tags_[i].value;
      if (!tags_[i].addressOf.empty())
        value = requiredSection(tags_[i].addressOf).vma;
      put(bytes, i * 16, tags_[i].tag, 8);
      put(bytes, i * 16 + 8, value, 8);
    }
  }

  void encodeSections() {
    stageInputSections();
    data(".dynstr") = dynstr_;
    if (executable_) {
      std::string interpreter = state_.options.dynamicLinker.empty()
          ? "/lib64/ld-linux-x86-64.so.2"
          : state_.options.dynamicLinker;
      auto& bytes = data(".interp");
      std::copy(interpreter.begin(), interpreter.end(), bytes.begin());
      bytes.back() = 0;
    }
    applyPlannedRelocations();
    buildGot();
    buildPlt();
    encodeRelocations(".rela.dyn", relaDynamic_);
    encodeRelocations(".rela.plt", relaPlt_);
    encodeSymbols();
    encodeHashes();
    encodeDynamic();
  }

  uint32_t sectionType(const OutputSection& output) const {
    if (output.bss) return 8;
    if (output.name == ".hash") return 5;
    if (output.name == ".gnu.hash") return 0x6ffffff6;
    if (output.name == ".dynsym") return 11;
    if (output.name == ".dynstr") return 3;
    if (output.name == ".rela.dyn" || output.name == ".rela.plt") return 4;
    if (output.name == ".dynamic") return 6;
    if (output.name == ".init_array") return 14;
    if (output.name == ".fini_array") return 15;
    if (output.name == ".preinit_array") return 16;
    if (output.flags & qbfd::sec::Note) return 7;
    return 1;
  }

  uint64_t sectionFlags(const OutputSection& output) const {
    uint64_t flags = 0;
    if (output.writable) flags |= 1;
    if (output.alloc) flags |= 2;
    if (output.executable) flags |= 4;
    if (output.flags & qbfd::sec::Strings) flags |= 0x20;
    return flags;
  }

  uint64_t sectionEntsize(const OutputSection& output) const {
    if (output.name == ".dynsym" ||
        output.name == ".rela.dyn" || output.name == ".rela.plt")
      return 24;
    if (output.name == ".dynamic") return 16;
    if (output.name == ".hash" || output.name == ".gnu.hash") return 4;
    if (output.name == ".plt") return 16;
    if (output.name == ".got" || output.name == ".got.plt" ||
        output.name == ".init_array" || output.name == ".fini_array" ||
        output.name == ".preinit_array")
      return 8;
    return 0;
  }

  void emitProgramHeader(Writer& writer, uint64_t offset,
                         const Segment& segment) {
    writer.put(offset, segment.type, 4);
    writer.put(offset + 4, segment.flags, 4);
    writer.put(offset + 8, segment.offset, 8);
    writer.put(offset + 16, segment.address, 8);
    writer.put(offset + 24, segment.address, 8);
    writer.put(offset + 32, segment.fileSize, 8);
    writer.put(offset + 40, segment.memorySize, 8);
    writer.put(offset + 48, segment.alignment, 8);
  }

  void emitImage(std::vector<uint8_t>& output) {
    std::vector<uint8_t> shstr{0};
    std::map<std::string, uint32_t> names;
    auto name = [&](const std::string& text) {
      auto found = names.find(text);
      if (found != names.end()) return found->second;
      uint32_t offset = uint32_t(shstr.size());
      shstr.insert(shstr.end(), text.begin(), text.end());
      shstr.push_back(0);
      names[text] = offset;
      return offset;
    };
    for (const auto& section : state_.outSections) name(section.name);
    name(".shstrtab");

    uint64_t fileEnd = 0;
    for (const auto& section : state_.outSections)
      if (!section.bss)
        fileEnd = std::max(fileEnd,
                           checkedAdd(section.fileOffset, section.fileSize));
    uint64_t shstrOffset = fileEnd;
    uint64_t shoff = alignUp(checkedAdd(shstrOffset, shstr.size()), 8);
    uint64_t shnum = 2 + state_.outSections.size();
    uint64_t total = checkedAdd(shoff, shnum * 64);
    output.assign(size_t(total), 0);
    Writer writer{output};

    output[0] = 0x7f; output[1] = 'E'; output[2] = 'L'; output[3] = 'F';
    output[4] = 2; output[5] = 1; output[6] = 1; output[7] = 0;
    writer.put(16, shared_ || pie_ ? 3 : 2, 2);
    writer.put(18, 62, 2);
    writer.put(20, 1, 4);
    writer.put(24, state_.entryAddr, 8);
    writer.put(32, 64, 8);
    writer.put(40, shoff, 8);
    writer.put(48, 0, 4);
    writer.put(52, 64, 2);
    writer.put(54, 56, 2);

    std::vector<Segment> headers;
    uint64_t headerCount = 1 + segments_.size() + 2 +
        (executable_ ? 1 : 0) + (state_.options.relro ? 1 : 0);
    headers.push_back(
        {6, 4, 64, checkedAdd(base_, 64),
         headerCount * 56, headerCount * 56, 8});  // PT_PHDR
    if (executable_) {
      const auto& interp = requiredSection(".interp");
      headers.push_back({3, 4, interp.fileOffset, interp.vma,
                         interp.fileSize, interp.size, 1});
    }
    headers.insert(headers.end(), segments_.begin(), segments_.end());
    const auto& dynamic = requiredSection(".dynamic");
    headers.push_back({2, 6, dynamic.fileOffset, dynamic.vma,
                       dynamic.fileSize, dynamic.size, 8});
    headers.push_back({0x6474e551, 6, 0, 0, 0, 0, 16});
    if (state_.options.relro)
      headers.push_back({0x6474e552, 4, relroAddress_ - base_, relroAddress_,
                         relroSize_, relroSize_, 1});
    writer.put(56, headers.size(), 2);
    writer.put(58, 64, 2);
    writer.put(60, shnum, 2);
    writer.put(62, shnum - 1, 2);
    for (size_t i = 0; i < headers.size(); ++i)
      emitProgramHeader(writer, 64 + i * 56, headers[i]);

    for (size_t i = 0; i < state_.outSections.size(); ++i) {
      const auto& section = state_.outSections[i];
      if (!section.bss && section.fileSize)
        writer.copy(section.fileOffset, staged_[i]);
    }
    std::copy(shstr.begin(), shstr.end(),
              output.begin() + ptrdiff_t(shstrOffset));

    auto emitSection = [&](uint64_t index, uint32_t sectionName,
                           uint32_t type, uint64_t flags, uint64_t address,
                           uint64_t fileOffset, uint64_t size, uint32_t link,
                           uint32_t info, uint64_t alignment,
                           uint64_t entsize) {
      uint64_t offset = shoff + index * 64;
      writer.put(offset, sectionName, 4);
      writer.put(offset + 4, type, 4);
      writer.put(offset + 8, flags, 8);
      writer.put(offset + 16, address, 8);
      writer.put(offset + 24, fileOffset, 8);
      writer.put(offset + 32, size, 8);
      writer.put(offset + 40, link, 4);
      writer.put(offset + 44, info, 4);
      writer.put(offset + 48, alignment, 8);
      writer.put(offset + 56, entsize, 8);
    };
    emitSection(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    uint32_t dynsym = requiredSection(".dynsym").index;
    uint32_t dynstr = requiredSection(".dynstr").index;
    for (const auto& section : state_.outSections) {
      uint32_t link = 0, info = 0;
      if (section.name == ".dynsym") {
        link = dynstr;
        info = 1;
      } else if (section.name == ".hash" ||
                 section.name == ".gnu.hash" ||
                 section.name == ".rela.dyn" ||
                 section.name == ".rela.plt") {
        link = dynsym;
      } else if (section.name == ".dynamic") {
        link = dynstr;
      }
      if (section.name == ".rela.plt" && this->section(".got.plt"))
        info = requiredSection(".got.plt").index;
      emitSection(section.index, names.at(section.name),
                  sectionType(section), sectionFlags(section), section.vma,
                  section.fileOffset, section.size, link, info,
                  std::max<uint64_t>(section.alignment, 1),
                  sectionEntsize(section));
    }
    emitSection(shnum - 1, names.at(".shstrtab"), 3, 0, 0, shstrOffset,
                shstr.size(), 0, 0, 1, 0);
  }

  void buildMap() {
    std::string map = "Native dynamic ELF link map\n";
    char line[192];
    for (const auto& output : state_.outSections) {
      std::snprintf(line, sizeof(line),
                    "  %-18s vma=0x%llx size=0x%llx off=0x%llx\n",
                    output.name.c_str(),
                    static_cast<unsigned long long>(output.vma),
                    static_cast<unsigned long long>(output.size),
                    static_cast<unsigned long long>(output.fileOffset));
      map += line;
    }
    map += "Dynamic symbols:\n";
    for (const auto& symbol : symbols_) {
      if (symbol.name.empty()) continue;
      std::snprintf(line, sizeof(line), "  %-32s 0x%llx\n",
                    symbol.name.c_str(),
                    static_cast<unsigned long long>(symbol.value));
      map += line;
    }
    state_.mapText = std::move(map);
  }
};

}  // namespace

bool emitDynamicElf(LinkState& state, std::vector<uint8_t>& out,
                    std::string& error) {
  try {
    DynamicImage(state).emit(out);
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

}  // namespace qld::detail
