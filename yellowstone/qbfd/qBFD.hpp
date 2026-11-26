// Header-only umbrella. All builtin formats are available without macros.
#pragma once
#include "qABI.hpp"
#include "qArchive.hpp"
#include "qBFDCore.hpp"
#include "qCOFF.hpp"
#include "qELF.hpp"
#include "qMachO.hpp"
#include "qPE.hpp"

namespace qbfd {

enum class ImageKind : uint8_t {
  Unknown = 0, Relocatable = 1, Executable = 2, SharedLibrary = 3,
  Core = 4, PositionIndependentExecutable = 5,
};

struct ImageInfo {
  Format format;
  Arch arch;
  Endian endian;
  uint8_t addressBits;
  ImageKind kind;
  abi::CallConv abi;
  uint64_t imageBase;
  uint64_t entryPoint;
};

inline abi::CallConv abi::convFor(const ObjectFile& object) {
  using enum CallConv;
  if (object.endian() != Endian::Little) return Unknown;
  const auto arch = object.arch();
  const bool wide = object.is64Bit();
  if (auto e = dynamic_cast<const elf::ELFFile*>(&object)) {
    // OSABI_NONE is common even on GNU/Linux. Other OSABI values may
    // identify contracts not modeled by these call descriptors.
    if (e->osABI() != 0 && e->osABI() != 3) return Unknown;
    if (arch == Arch::X86_64 && wide) return SysV_AMD64;
    if (arch == Arch::AArch64 && wide) return Aapcs64;
    // EF_RISCV_FLOAT_ABI_DOUBLE only; exclude the reduced-register ABI.
    if ((e->flags() & 0xe) == 4) {
      if (arch == Arch::RISCV64 && wide) return Riscv64;
      if (arch == Arch::RISCV32 && !wide) return Riscv32;
    }
  } else if (auto m = dynamic_cast<const macho::MachOFile*>(&object)) {
    if (arch == Arch::X86_64 && wide) return SysV_AMD64;
    // arm64e has a distinct authenticated function-pointer representation.
    if (arch == Arch::AArch64 && wide && (m->cpuSubtype() & 0xffffff) != 2)
      return DarwinArm64;
  } else if ((object.format() == Format::PE || object.format() == Format::COFF) &&
             arch == Arch::X86_64 && wide) {
    return Win64;
  }
  return Unknown;
}

inline ImageInfo imageInfo(const ObjectFile& object) {
  ImageKind kind = ImageKind::Unknown;
  switch (object.fileType()) {
    case FileType::Relocatable: kind = ImageKind::Relocatable; break;
    case FileType::Executable: kind = ImageKind::Executable; break;
    case FileType::SharedObject: kind = ImageKind::SharedLibrary; break;
    case FileType::Core: kind = ImageKind::Core; break;
    default: break;
  }
  if (auto e = dynamic_cast<const elf::ELFFile*>(&object); e && e->isPie())
    kind = ImageKind::PositionIndependentExecutable;
  if (auto m = dynamic_cast<const macho::MachOFile*>(&object);
      m && kind == ImageKind::Executable && (m->flags() & 0x200000))
    kind = ImageKind::PositionIndependentExecutable;
  return {object.format(), object.arch(), object.endian(),
          uint8_t(object.is64Bit() ? 64 : 32), kind, abi::convFor(object),
          object.imageBase(), object.entryPoint()};
}

namespace detail {
inline std::vector<LinkSymbol> elfLinkSymbols(const elf::ELFFile& object,
                                              bool imported) {
  std::vector<LinkSymbol> result;
  for (const auto& s : object.dynamicSymbols()) {
    if (s.name.empty() || !(s.flags & (imported ? sym::Imported : sym::Exported)))
      continue;
    LinkSymbol link;
    link.name = s.name;
    link.weak = s.binding == SymbolBinding::Weak;
    if (!imported && s.kind != SymbolKind::Tls && s.kind != SymbolKind::Common)
      link.address = s.value;
    if (auto* v = object.symbolVersion(s.index)) {
      link.library = v->library;
      link.version = v->name;
      link.defaultVersion = v->defaultVersion;
    }
    result.push_back(std::move(link));
  }
  return result;
}
}  // namespace detail

inline std::vector<LinkSymbol> exports(const ObjectFile& object) {
  if (auto e = dynamic_cast<const elf::ELFFile*>(&object))
    return detail::elfLinkSymbols(*e, false);
  if (auto m = dynamic_cast<const macho::MachOFile*>(&object))
    return m->exportedSymbols();
  std::vector<LinkSymbol> result;
  if (auto p = dynamic_cast<const pe::PEFile*>(&object))
    for (const auto& entry : p->exports()) {
      LinkSymbol link;
      link.name = entry.name;
      link.ordinal = entry.ordinal;
      if (entry.forwarder.empty())
        link.address = entry.address;
      else {
        link.forwarded = true;
        auto dot = entry.forwarder.rfind('.');
        if (dot == std::string::npos)
          link.target = entry.forwarder;
        else {
          link.library = entry.forwarder.substr(0, dot);
          link.target = entry.forwarder.substr(dot + 1);
        }
      }
      result.push_back(std::move(link));
    }
  return result;
}

inline std::vector<LinkSymbol> imports(const ObjectFile& object) {
  if (auto e = dynamic_cast<const elf::ELFFile*>(&object))
    return detail::elfLinkSymbols(*e, true);
  if (auto m = dynamic_cast<const macho::MachOFile*>(&object))
    return m->importedSymbols();
  std::vector<LinkSymbol> result;
  if (auto p = dynamic_cast<const pe::PEFile*>(&object))
    for (const auto& entry : p->imports()) {
      LinkSymbol link;
      link.name = entry.name;
      link.library = entry.dll;
      link.address = entry.iatAddress;
      if (link.name.empty()) link.ordinal = entry.ordinal;
      result.push_back(std::move(link));
    }
  return result;
}

inline void registerBuiltinTargets() {
  static const bool initialized = [] {
    auto& r = Registry::instance();
    elf::registerTargets(r);
    pe::registerTargets(r);
    coff::registerTargets(r);
    macho::registerTargets(r);
    return true;
  }();
  (void)initialized;
}

// Format detection without a full parse
inline Format detectFormat(std::span<const uint8_t> d) {
  if (d.size() >= 4 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' &&
      d[3] == 'F')
    return Format::ELF;
  if (d.size() >= 4) {
    uint32_t m = detail::Reader{d}.u32(0);
    if (m == 0xFEEDFACE || m == 0xFEEDFACF || m == 0xCEFAEDFE ||
        m == 0xCFFAEDFE || m == 0xCAFEBABE || m == 0xBEBAFECA ||
        m == 0xCAFEBABF || m == 0xBFBAFECA)
      return Format::MachO;
  }
  if (d.size() >= 2 && d[0] == 'M' && d[1] == 'Z') return Format::PE;
  if (d.size() >= 2) {
    uint16_t m = d[0] | d[1] << 8;
    if (coff::knownMachine(m) ||
        coff::COFFFile::isBigObjHeader(coff::Reader{d}))
      return Format::COFF;
  }
  return Format::Unknown;
}
inline Expected<Opened> openImpl(std::shared_ptr<Buffer> buf,
                                 const std::filesystem::path& label,
                                 const Target* forced) {
  registerBuiltinTargets();
  auto data = buf->span();

  const Target* t = forced;
  if (!t) {
    auto matches = Registry::instance().match(data);
    if (matches.empty())
      return Error{Error::Code::UnknownFormat,
                   "no target recognises " + label.string()};
    if (matches.size() > 1)
      return Error{Error::Code::Ambiguous,
                   "multiple targets match " + label.string()};
    t = matches[0];
  }

  auto obj = t->open(data, label);
  if (!obj) return obj.error();
  if (!*obj)
    return Error{Error::Code::Malformed, "backend returned a null object"};
  (*obj)->retainBuffer(buf);
  return Opened{std::move(buf), std::move(*obj)};
}

inline Expected<Opened> open(const std::filesystem::path& path) {
  auto buf = Buffer::fromFile(path);
  if (!buf) return buf.error();
  return openImpl(*buf, path, nullptr);
}

inline Expected<Opened> open(std::vector<uint8_t> bytes,
                             const std::filesystem::path& label) {
  return openImpl(Buffer::fromBytes(std::move(bytes)), label, nullptr);
}

inline Expected<Opened> openMachOSliceImpl(std::shared_ptr<Buffer> buf,
                                          Arch arch,
                                          const std::filesystem::path& label) {
  if (arch == Arch::Unknown)
    return Error{Error::Code::Unsupported, "select a concrete Mach-O architecture"};
  auto bytes = buf->span();
  auto selected = [&]() -> Expected<std::unique_ptr<ObjectFile>> {
    if (!macho::isFat(bytes)) {
      auto object = macho::MachOFile::open(bytes, label);
      if (object && (*object)->arch() != arch)
        return Error{Error::Code::Unsupported, "thin Mach-O architecture mismatch"};
      return object;
    }
    auto slices = macho::slices(bytes);
    if (!slices) return slices.error();
    const macho::FatSlice* match = nullptr;
    for (const auto& slice : *slices)
      if (macho::archFromCpu(slice.cpuType) == arch) {
        if (match)
          return Error{Error::Code::Ambiguous, "multiple Mach-O slices for architecture"};
        match = &slice;
      }
    if (!match) return Error{Error::Code::Unsupported, "no matching Mach-O slice"};
    auto object = macho::MachOFile::open(bytes.subspan(match->offset, match->size),
                                        label);
    if (object) {
      const auto& m = static_cast<const macho::MachOFile&>(**object);
      if (m.cpuType() != match->cpuType || m.cpuSubtype() != match->cpuSubtype)
        return Error{Error::Code::Malformed, "universal slice CPU mismatch"};
    }
    return object;
  }();
  if (!selected) return selected.error();
  (*selected)->retainBuffer(buf);
  return Opened{std::move(buf), std::move(*selected)};
}

inline Expected<Opened> openMachOSlice(std::vector<uint8_t> bytes, Arch arch,
                                     const std::filesystem::path& label = {}) {
  return openMachOSliceImpl(Buffer::fromBytes(std::move(bytes)), arch, label);
}

inline Expected<Opened> openMachOSlice(const std::filesystem::path& path, Arch arch) {
  auto buf = Buffer::fromFile(path);
  if (!buf) return buf.error();
  return openMachOSliceImpl(*buf, arch, path);
}

inline Expected<Opened> openAs(const std::filesystem::path& path,
                               std::string_view targetName) {
  registerBuiltinTargets();
  const Target* t = Registry::instance().find(targetName);
  if (!t)
    return Error{Error::Code::Unsupported,
                 "unknown target " + std::string(targetName)};
  auto buf = Buffer::fromFile(path);
  if (!buf) return buf.error();
  return openImpl(*buf, path, t);
}

}  // namespace qbfd

namespace qbfd {
inline Expected<Opened> openAs(std::vector<uint8_t> bytes,
                               std::string_view targetName,
                               const std::filesystem::path& label = {}) {
  registerBuiltinTargets();
  auto target = Registry::instance().find(targetName);
  if (!target)
    return Error{Error::Code::Unsupported,
                 "unknown target " + std::string(targetName)};
  return openImpl(Buffer::fromBytes(std::move(bytes)), label, target);
}
namespace query {
inline auto read() {
  return ::dsl::pipe([](auto source) { return qbfd::open(std::move(source)); });
}
inline auto as(std::string target) {
  return ::dsl::pipe([target = std::move(target)](auto source) {
    return qbfd::openAs(std::move(source), target);
  });
}
inline auto requireFormat(Format format) {
  return ::dsl::pipe([format](Expected<Opened> input) -> Expected<Opened> {
    if (!input) return input.error();
    if (input->object->format() != format)
      return Error{Error::Code::UnknownFormat, "unexpected object format"};
    return input;
  });
}
// Lift a query into the error-aware open pipeline; never dereference a failed
// open.
template <class Stage>
auto inspect(Stage stage) {
  return ::dsl::pipe([stage = std::move(stage)](Expected<Opened> input) {
    using Value = std::decay_t<decltype(*input->object | stage)>;
    if (!input) return Expected<Value>(input.error());
    return Expected<Value>(*input->object | stage);
  });
}
}  // namespace query
}  // namespace qbfd
