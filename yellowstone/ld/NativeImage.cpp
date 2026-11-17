// Small, deliberately bounded native PE/COFF and Mach-O static image linker.
// qBFD supplies decoded inputs; no host structs or external tools define output.
#include "Backend.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace qld::detail {
namespace {
constexpr uint64_t imageLimit = 256 * 1024 * 1024;

uint64_t add(uint64_t a, uint64_t b) {
  if (b > imageLimit || a > imageLimit - b)
    throw std::runtime_error("native image exceeds 256 MiB size limit");
  return a + b;
}
uint64_t align(uint64_t n, uint64_t a) {
  if (!a || (a & (a - 1)) || a > 65536)
    throw std::runtime_error("unsupported native section alignment");
  return add(n, a - 1) & ~(a - 1);
}
void put(std::vector<uint8_t>& bytes, uint64_t off, uint64_t value, unsigned width) {
  if (off > bytes.size() || width > bytes.size() - off)
    throw std::runtime_error("native output write outside image");
  for (unsigned i = 0; i < width; ++i) bytes[size_t(off + i)] = uint8_t(value >> (8 * i));
}
uint64_t get(const std::vector<uint8_t>& bytes, uint64_t off, unsigned width) {
  if (off > bytes.size() || width > bytes.size() - off)
    throw std::runtime_error("native relocation outside section");
  uint64_t value = 0;
  for (unsigned i = 0; i < width; ++i) value |= uint64_t(bytes[size_t(off + i)]) << (8 * i);
  return value;
}
void name(std::vector<uint8_t>& bytes, uint64_t off, const std::string& text, size_t size) {
  if (text.size() > size) throw std::runtime_error("native section name too long: " + text);
  if (off > bytes.size() || size > bytes.size() - off)
    throw std::runtime_error("native name outside image");
  std::copy(text.begin(), text.end(), bytes.begin() + ptrdiff_t(off));
}

struct Section {
  std::string name;
  bool code = false, writable = false, bss = false;
  uint64_t alignment = 1, size = 0, address = 0, file = 0, rawSize = 0;
  uint32_t nativeFlags = 0;
  std::vector<uint8_t> data;
};
struct Contribution {
  size_t object;
  qbfd::Section source;
  size_t output;
  uint64_t offset;
  std::vector<qbfd::Relocation> relocations;
};

class Image {
 public:
  explicit Image(LinkState& state) : s(state), pe(s.format == qbfd::Format::COFF) {}
  void link(std::vector<uint8_t>& out) {
    if (s.options.mode != Mode::Exec || s.options.pie)
      throw std::runtime_error("unsupported native PE/Mach-O output kind");
    if (s.options.allowUndefined)
      throw std::runtime_error("native PE/Mach-O images require resolved symbols");
    if (s.options.gcSections)
      throw std::runtime_error("native PE/Mach-O section GC is not yet supported");
    if (s.endian != qbfd::Endian::Little ||
        (s.arch != qbfd::Arch::X86_64 && !(pe && s.arch == qbfd::Arch::X86)))
      throw std::runtime_error("unsupported native PE/Mach-O architecture (x86-64, PE x86 supported)");
    if (!s.options.emulator.empty()) {
      auto e = s.options.emulator;
      bool match = s.arch == qbfd::Arch::X86_64 ?
          e == "x86_64" || (pe ? e == "x64" || e == "i386pep" : false) :
          e == "x86" || e == "i386pe";
      if (!match) throw std::runtime_error("native emulation/architecture mismatch: " + e);
    }
    base = s.options.hasBase ? s.options.base :
           pe ? (s.wide ? UINT64_C(0x140000000) : 0x400000) : UINT64_C(0x100000000);
    if (base % (pe ? 65536 : 4096) || base > UINT64_MAX - imageLimit ||
        (!s.wide && base > UINT32_MAX - imageLimit))
      throw std::runtime_error("invalid native image base");
    collect();
    for (const auto& section : sections)
      if (base % section.alignment)
        throw std::runtime_error("native image base does not satisfy section alignment");
    if (pe) emitPE(out); else emitMachO(out);
    std::ostringstream map;
    map << "Native " << (pe ? "PE" : "Mach-O") << " link map\n";
    for (const auto& section : sections)
      map << section.name << " address=0x" << std::hex << section.address
          << " size=0x" << section.size << '\n';
    for (const auto& [symbol, address] : s.symAddr)
      map << symbol << " 0x" << std::hex << address << '\n';
    s.mapText = map.str();
  }

 private:
  LinkState& s;
  bool pe;
  uint64_t base = 0;
  std::vector<Section> sections;
  std::vector<Contribution> contributions;
  std::map<std::string, std::pair<size_t, uint64_t>> commons;

  void collect() {
    // Sorting COFF $ suffixes implements ordered subsections without COMDAT
    // selection. Unsupported semantic sections must not just be copied.
    std::vector<std::pair<size_t, qbfd::Section>> inputs;
    for (size_t oi = 0; oi < s.objects.size(); ++oi) {
      auto& object = *s.objects[oi].object;
      for (const auto& section : object.sections()) {
        // qBFD classifies linker-info sections as debug/non-alloc. Directives
        // still carry required semantics and must be rejected before stripping.
        if (pe && section.name == ".drectve" && section.size)
          throw std::runtime_error("native COFF directives/imports/exports are not yet supported");
        if (section.flags & qbfd::sec::Debug) continue;
        if (pe && (section.name == ".llvm_addrsig" || section.nativeFlags & 0x800)) continue;
        if (!section.size) continue;
        if (section.flags & qbfd::sec::Tls ||
            section.name.starts_with(".tls") || section.name.starts_with(".CRT") ||
            (!pe && (section.nativeType != 0 && section.nativeType != 1 &&
                     section.nativeType != 2)))
          throw std::runtime_error(s.objects[oi].label + ": unsupported native section semantics: " + section.name);
        if (pe && (section.nativeFlags & 0x1000))
          throw std::runtime_error("native COFF COMDAT selection is not yet supported");
        if (!pe && (section.name == "__compact_unwind" || section.name == "__eh_frame" ||
                    section.name == "__mod_init_func" || section.name == "__mod_term_func"))
          throw std::runtime_error("native Mach-O unwind/initializer processing is not yet supported: " + section.name);
        inputs.emplace_back(oi, section);
      }
      for (const auto& symbol : object.symbols())
        if (symbol.kind == qbfd::SymbolKind::Indirect ||
            symbol.binding == qbfd::SymbolBinding::Weak)
          throw std::runtime_error("native PE/Mach-O weak/indirect symbols are not yet supported");
    }
    if (pe) std::stable_sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) {
      return a.second.name < b.second.name;
    });
    for (const auto& [oi, input] : inputs) {
      auto outputName = input.name;
      if (pe) outputName = outputName.substr(0, outputName.find('$'));
      bool bss = input.flags & qbfd::sec::Bss;
      bool code = input.flags & (qbfd::sec::Code | qbfd::sec::Exec);
      // MH_OBJECT's single segment has RWX protection for all sections. It is
      // not a statement that its code/constant sections require write access.
      bool writable = pe ? bool(input.flags & qbfd::sec::Writable) :
          !code && input.name != "__const" && input.name != "__cstring" && input.name != "__literal8";
      auto it = std::find_if(sections.begin(), sections.end(), [&](const Section& section) {
        return section.name == outputName;
      });
      if (it == sections.end()) {
        Section section;
        section.name = outputName; section.bss = bss; section.code = code;
        section.writable = writable; section.nativeFlags = uint32_t(input.nativeFlags);
        sections.push_back(std::move(section)); it = sections.end() - 1;
      } else if (it->bss != bss || it->code != code || it->writable != writable)
        throw std::runtime_error("incompatible native section contributions: " + outputName);
      it->alignment = std::max(it->alignment, std::max(input.alignment, UINT64_C(1)));
      auto offset = align(it->size, std::max(input.alignment, UINT64_C(1)));
      it->size = add(offset, input.size);
      if (!bss) {
        auto data = s.objects[oi].object->sectionContents(input);
        if (!data || data->size() != input.size)
          throw std::runtime_error("cannot read native section: " + input.name);
        it->data.resize(size_t(it->size));
        std::copy(data->begin(), data->end(), it->data.begin() + ptrdiff_t(offset));
      }
      auto relocs = s.objects[oi].object->relocations(input);
      if (!relocs) throw std::runtime_error("cannot read native relocations: " + input.name);
      contributions.push_back({oi, input, size_t(it - sections.begin()), offset, std::move(*relocs)});
    }
    for (const auto& [symbol, def] : s.defs) {
      if (!def.common) continue;
      auto it = std::find_if(sections.begin(), sections.end(), [](const Section& section) {
        return section.bss && section.writable;
      });
      if (it == sections.end()) {
        Section section;
        section.name = pe ? ".bss" : "__bss"; section.bss = section.writable = true;
        sections.push_back(std::move(section)); it = sections.end() - 1;
      }
      it->alignment = std::max(it->alignment, def.commonAlign);
      auto offset = align(it->size, def.commonAlign);
      it->size = add(offset, def.commonSize);
      commons[symbol] = {size_t(it - sections.begin()), offset};
    }
    if (sections.empty() || sections.size() > (pe ? 96u : 255u))
      throw std::runtime_error("unsupported native image section count");
  }

  const Contribution& contribution(size_t object, uint32_t section) const {
    for (const auto& c : contributions)
      if (c.object == object && c.source.index == section) return c;
    throw std::runtime_error("native relocation/symbol refers to a removed section");
  }
  uint64_t defined(size_t object, const qbfd::Symbol& symbol) const {
    if (symbol.flags & qbfd::sym::Absolute) return symbol.value;
    if (!symbol.section) throw std::runtime_error("native symbol has no definition: " + symbol.name);
    const auto& c = contribution(object, *symbol.section);
    // Mach-O n_value is an input virtual address, not a section offset.
    auto origin = c.source.vma;
    if (symbol.value < origin || symbol.value - origin > c.source.size)
      throw std::runtime_error("native symbol outside input section: " + symbol.name);
    return sections[c.output].address + c.offset + symbol.value - origin;
  }
  uint64_t symbolAddress(size_t object, const qbfd::Symbol& symbol) const {
    if (symbol.binding != qbfd::SymbolBinding::Local) {
      auto it = s.symAddr.find(symbol.name);
      if (it != s.symAddr.end()) return it->second;
    }
    if (symbol.flags & qbfd::sym::Undefined)
      throw std::runtime_error("undefined native symbol: " + symbol.name);
    return defined(object, symbol);
  }
  void resolve() {
    s.symAddr.clear();
    for (const auto& [symbol, def] : s.defs) {
      if (def.common) {
        const auto& [index, offset] = commons.at(symbol);
        s.symAddr[symbol] = sections[index].address + offset;
      } else s.symAddr[symbol] = defined(def.objectIndex, def.symbol);
    }
    auto it = s.symAddr.find(s.options.entry);
    if (it == s.symAddr.end()) throw std::runtime_error("undefined native entry symbol: " + s.options.entry);
    s.entryAddr = it->second;
    bool executableEntry = false;
    for (const auto& section : sections)
      if (section.code && s.entryAddr >= section.address && s.entryAddr - section.address < section.size)
        executableEntry = true;
    if (!executableEntry) throw std::runtime_error("native entry is not in executable code");
  }

  void relocate() {
    for (const auto& c : contributions) {
      auto& output = sections[c.output];
      const auto& object = *s.objects[c.object].object;
      for (const auto& r : c.relocations) {
        if (pe && r.nativeType == 0) continue; // IMAGE_REL_*_ABSOLUTE padding
        if (output.bss || r.scattered || !r.bitSize || r.bitSize % 8)
          throw std::runtime_error("unsupported native relocation encoding");
        unsigned width = r.bitSize / 8;
        if (width != 2 && width != 4 && width != 8)
          throw std::runtime_error("unsupported native relocation width");
        if (r.offset > c.source.size || width > c.source.size - r.offset)
          throw std::runtime_error("native relocation outside input section");
        auto loc = c.offset + r.offset;
        uint64_t P = output.address + loc, S = 0;
        const qbfd::Symbol *symbol = nullptr;
        if (r.symbol) {
          const auto& symbols = object.symbols();
          if (*r.symbol >= symbols.size()) throw std::runtime_error("bad native relocation symbol");
          symbol = &symbols[*r.symbol];
          S = symbolAddress(c.object, *symbol);
        } else if (!pe && r.targetSection) {
          const auto& target = contribution(c.object, *r.targetSection);
          S = sections[target.output].address + target.offset - target.source.vma;
        } else throw std::runtime_error("native relocation has no target");
        uint64_t A = get(output.data, loc, width), value = 0;
        if (pe && r.hasAddend) A = uint64_t(r.addend);
        bool signedResult = false;
        if (pe) {
          uint32_t type = r.nativeType;
          bool x64 = s.arch == qbfd::Arch::X86_64;
          if ((x64 && type == 1) || (!x64 && type == 6)) value = S + A;
          else if (x64 && type == 2) value = S + A;
          else if ((x64 && type == 3) || (!x64 && type == 7)) value = S + A - base;
          else if ((x64 && type >= 4 && type <= 9) || (!x64 && type == 20)) {
            value = S + uint64_t(int64_t(int32_t(A))) - P - 4 - (x64 ? type - 4 : 0);
            signedResult = true;
          } else if ((x64 && (type == 10 || type == 11)) ||
                     (!x64 && (type == 10 || type == 11))) {
            if (!symbol)
              throw std::runtime_error("native COFF section relocation needs a defined symbol");
            // Global definitions can be supplied by another object.
            size_t oi = c.object;
            const qbfd::Symbol *definition = symbol;
            if (symbol->binding != qbfd::SymbolBinding::Local) {
              auto it = s.defs.find(symbol->name);
              if (it != s.defs.end()) { oi = it->second.objectIndex; definition = &it->second.symbol; }
            }
            if (!definition->section) throw std::runtime_error("native COFF section relocation without section");
            const auto& target = contribution(oi, *definition->section);
            value = type == 10 ? target.output + 1 + A : S + A - sections[target.output].address;
          } else throw std::runtime_error("unsupported native COFF relocation: " + r.typeName);
        } else {
          // X86_64_RELOC_UNSIGNED, SIGNED, BRANCH, SIGNED_1/_2/_4 only.
          if (r.nativeType == 0 && !r.pcRelative) value = S + A;
          else if ((r.nativeType == 1 || r.nativeType == 2 ||
                    r.nativeType == 6 || r.nativeType == 7 || r.nativeType == 8) &&
                   r.pcRelative && width == 4) {
            // Local relocations contain the old target-place distance;
            // external ones contain the signed addend INCLUDING any trailing
            // immediate bias (SIGNED_1/_2/_4). Never subtract that bias twice.
            value = S + uint64_t(int64_t(int32_t(A))) - P - 4;
            if (!r.symbol) value += c.source.vma + r.offset + 4;
            signedResult = true;
          } else throw std::runtime_error("unsupported native Mach-O relocation: " + r.typeName);
        }
        if ((signedResult && int64_t(value) != int64_t(int32_t(value))) ||
            (!signedResult && width < 8 && value >> (width * 8)))
          throw std::runtime_error("native relocation truncated to fit: " + r.typeName);
        put(output.data, loc, value, width);
      }
    }
  }

  void copySections(std::vector<uint8_t>& out) const {
    for (const auto& section : sections)
      if (!section.bss) std::copy(section.data.begin(), section.data.end(),
                                 out.begin() + ptrdiff_t(section.file));
  }
  void emitPE(std::vector<uint8_t>& out) {
    const uint64_t optionalSize = s.wide ? 240 : 224;
    const uint64_t table = 128 + 24 + optionalSize;
    uint64_t headerSize = align(add(table, sections.size() * 40), 512);
    uint64_t file = headerSize, rva = align(headerSize, 4096);
    uint64_t codeSize = 0, dataSize = 0, bssSize = 0, baseCode = 0, baseData = 0;
    for (auto& section : sections) {
      if (section.alignment > 4096) throw std::runtime_error("native PE section alignment exceeds 4096");
      section.address = base + rva;
      section.file = section.bss ? 0 : file;
      section.rawSize = section.bss ? 0 : align(section.size, 512);
      file = add(file, section.rawSize);
      rva = align(add(rva, section.size), 4096);
      if (section.code) { codeSize += section.rawSize; if (!baseCode) baseCode = section.address - base; }
      else if (section.bss) bssSize += section.size;
      else { dataSize += section.rawSize; if (!baseData) baseData = section.address - base; }
    }
    resolve(); relocate();
    out.assign(size_t(file), 0);
    out[0] = 'M'; out[1] = 'Z'; put(out, 60, 128, 4);
    put(out, 128, 0x4550, 4);
    put(out, 132, s.wide ? 0x8664 : 0x14c, 2);
    put(out, 134, sections.size(), 2);
    put(out, 148, optionalSize, 2);
    put(out, 150, 0x0003 | (s.wide ? 0x20 : 0x100), 2); // executable, relocations stripped
    uint64_t o = 152;
    put(out, o, s.wide ? 0x20b : 0x10b, 2);
    put(out, o + 4, codeSize, 4); put(out, o + 8, dataSize, 4); put(out, o + 12, bssSize, 4);
    put(out, o + 16, s.entryAddr - base, 4); put(out, o + 20, baseCode, 4);
    if (!s.wide) put(out, o + 24, baseData, 4);
    put(out, o + (s.wide ? 24 : 28), base, s.wide ? 8 : 4);
    put(out, o + 32, 4096, 4); put(out, o + 36, 512, 4);
    put(out, o + 40, 6, 2); put(out, o + 48, 6, 2);
    put(out, o + 56, rva, 4); put(out, o + 60, headerSize, 4);
    put(out, o + 68, 3, 2); put(out, o + 70, 0x100, 2); // console, NX (no ASLR without .reloc)
    auto word = s.wide ? 8u : 4u;
    put(out, o + 72, 1024 * 1024, word); put(out, o + 72 + word, 4096, word);
    put(out, o + 72 + 2 * word, 1024 * 1024, word); put(out, o + 72 + 3 * word, 4096, word);
    put(out, o + (s.wide ? 108 : 92), 16, 4);
    for (size_t i = 0; i < sections.size(); ++i) {
      const auto& section = sections[i];
      auto sh = table + i * 40;
      name(out, sh, section.name, 8);
      put(out, sh + 8, section.size, 4); put(out, sh + 12, section.address - base, 4);
      put(out, sh + 16, section.rawSize, 4); put(out, sh + 20, section.file, 4);
      uint32_t flags = 0x40000000 | (section.writable ? 0x80000000 : 0) |
          (section.code ? 0x20000020 : section.bss ? 0x80 : 0x40);
      put(out, sh + 36, flags, 4);
      if (section.name == ".pdata") {
        if (!s.wide || section.size % 12) throw std::runtime_error("unsupported native PE exception table");
        // Runtime function entries must be ordered by BeginAddress.
        std::vector<std::array<uint8_t, 12>> entries(size_t(section.size / 12));
        for (size_t n = 0; n < entries.size(); ++n)
          std::copy_n(section.data.begin() + ptrdiff_t(n * 12), 12, entries[n].begin());
        auto begin = [](const auto& e) { return uint32_t(e[0]) | uint32_t(e[1]) << 8 |
                                               uint32_t(e[2]) << 16 | uint32_t(e[3]) << 24; };
        std::sort(entries.begin(), entries.end(), [&](const auto& a, const auto& b) { return begin(a) < begin(b); });
        auto& data = sections[i].data;
        for (size_t n = 0; n < entries.size(); ++n)
          std::copy(entries[n].begin(), entries[n].end(), data.begin() + ptrdiff_t(n * 12));
        put(out, o + 112 + 3 * 8, section.address - base, 4);
        put(out, o + 112 + 3 * 8 + 4, section.size, 4);
      }
    }
    copySections(out);
  }

  void emitMachO(std::vector<uint8_t>& out) {
    // Static MH_EXECUTE with LC_UNIXTHREAD, not a fake dyld image. No LC_MAIN,
    // dylib commands, imports, chained fixups, code signature or PIE claims.
    uint64_t commandSize = 4 * 72 + sections.size() * 80 + 184 + 24;
    uint64_t cursor = align(add(32, commandSize), 4096);
    std::vector<size_t> text, data;
    for (size_t i = 0; i < sections.size(); ++i)
      (sections[i].writable ? data : text).push_back(i);
    auto place = [&](const std::vector<size_t>& list) {
      for (size_t i : list) {
        auto& section = sections[i];
        cursor = align(cursor, section.alignment);
        section.file = section.bss ? 0 : cursor; section.address = base + cursor;
        cursor = add(cursor, section.size);
      }
    };
    place(text);
    uint64_t dataStart = align(cursor, 4096);
    cursor = dataStart;
    // Zerofill must be last so that later file data cannot overlap it.
    std::stable_sort(data.begin(), data.end(), [&](size_t a, size_t b) { return sections[a].bss < sections[b].bss; });
    place(data);
    uint64_t dataEnd = cursor, dataFileEnd = dataStart;
    for (size_t i : data) if (!sections[i].bss)
      dataFileEnd = std::max(dataFileEnd, sections[i].file + sections[i].size);
    uint64_t linkStart = align(dataFileEnd, 4096), linkAddress = align(dataEnd, 4096);
    resolve(); relocate();
    std::vector<uint8_t> strings{0};
    std::vector<std::tuple<uint32_t, uint8_t, uint64_t>> symbols;
    std::vector<uint8_t> ordinals(sections.size());
    unsigned next = 1;
    for (const auto& list : {text, data}) for (size_t i : list) ordinals[i] = uint8_t(next++);
    for (const auto& [symbol, address] : s.symAddr) {
      uint8_t ordinal = 0;
      // Section order follows load-command order, not collection order.
      const auto& def = s.defs.at(symbol);
      if (def.common) ordinal = ordinals[commons.at(symbol).first];
      else if (!(def.symbol.flags & qbfd::sym::Absolute) && def.symbol.section)
        ordinal = ordinals[contribution(def.objectIndex, *def.symbol.section).output];
      auto offset = uint32_t(strings.size());
      strings.insert(strings.end(), symbol.begin(), symbol.end()); strings.push_back(0);
      symbols.emplace_back(offset, ordinal, address);
    }
    uint64_t stringOffset = add(linkStart, symbols.size() * 16);
    uint64_t end = add(stringOffset, strings.size());
    out.assign(size_t(end), 0);
    put(out, 0, 0xfeedfacf, 4); put(out, 4, 0x1000007, 4); put(out, 8, 3, 4);
    put(out, 12, 2, 4); put(out, 16, 6, 4); put(out, 20, commandSize, 4);
    put(out, 24, 1, 4); // MH_NOUNDEFS; no MH_DYLDLINK or MH_PIE
    uint64_t command = 32;
    auto segment = [&](const std::string& segmentName, uint64_t address, uint64_t memorySize,
                       uint64_t fileOffset, uint64_t fileSize, uint32_t prot,
                       const std::vector<size_t>& list) {
      auto size = 72 + list.size() * 80;
      put(out, command, 0x19, 4); put(out, command + 4, size, 4);
      name(out, command + 8, segmentName, 16);
      put(out, command + 24, address, 8); put(out, command + 32, memorySize, 8);
      put(out, command + 40, fileOffset, 8); put(out, command + 48, fileSize, 8);
      put(out, command + 56, prot, 4); put(out, command + 60, prot, 4);
      put(out, command + 64, list.size(), 4);
      for (size_t i = 0; i < list.size(); ++i) {
        const auto& section = sections[list[i]];
        auto sh = command + 72 + i * 80;
        name(out, sh, section.name, 16); name(out, sh + 16, segmentName, 16);
        put(out, sh + 32, section.address, 8); put(out, sh + 40, section.size, 8);
        put(out, sh + 48, section.file, 4);
        unsigned power = 0;
        while ((UINT64_C(1) << power) < section.alignment) ++power;
        put(out, sh + 52, power, 4);
        put(out, sh + 64, section.bss ? 1 : section.code ? 0x80000400 : section.nativeFlags, 4);
      }
      command += size;
    };
    segment("__PAGEZERO", 0, base, 0, 0, 0, {});
    segment("__TEXT", base, dataStart, 0, dataStart, 5, text);
    segment("__DATA", base + dataStart, align(dataEnd - dataStart, 4096),
            dataStart, dataFileEnd - dataStart, 3, data);
    segment("__LINKEDIT", base + linkAddress, align(end - linkStart, 4096),
            linkStart, end - linkStart, 1, {});
    put(out, command, 5, 4); put(out, command + 4, 184, 4);
    put(out, command + 8, 4, 4); put(out, command + 12, 42, 4);
    put(out, command + 16 + 16 * 8, s.entryAddr, 8); // x86_THREAD_STATE64.rip
    command += 184;
    put(out, command, 2, 4); put(out, command + 4, 24, 4);
    put(out, command + 8, linkStart, 4); put(out, command + 12, symbols.size(), 4);
    put(out, command + 16, stringOffset, 4); put(out, command + 20, strings.size(), 4);
    for (size_t i = 0; i < symbols.size(); ++i) {
      auto [offset, section, address] = symbols[i];
      auto at = linkStart + i * 16;
      put(out, at, offset, 4); put(out, at + 4, section ? 0xf : 3, 1);
      put(out, at + 5, section, 1); put(out, at + 8, address, 8);
    }
    std::copy(strings.begin(), strings.end(), out.begin() + ptrdiff_t(stringOffset));
    copySections(out);
  }
};
} // namespace

bool linkNativeImage(LinkState& state, std::vector<uint8_t>& out, std::string& error) {
  try { Image(state).link(out); return true; }
  catch (const std::exception& e) { error = e.what(); return false; }
}
} // namespace qld::detail
