// The only ArchBFD translation unit that depends on the qBFD readers.
#include "BFDBinABI.h"
#include "qBFD.hpp"

#include <cstring>
#include <new>

struct archbfd_object {
  std::unique_ptr<qbfd::ObjectFile> file;
  std::vector<std::vector<qbfd::Relocation>> relocations;
  std::vector<std::string> dependencies;
  std::string target;
};
struct archbfd_archive {
  qbfd::OpenedArchive opened;
};

namespace {
// Any qBFD enum/flag change must be reconciled with the published ABI.
#define SAME(native, abi) static_assert(uint32_t(native) == uint32_t(abi))
SAME(qbfd::Format::Unknown, ARCHBFD_FORMAT_UNKNOWN);
SAME(qbfd::Format::ELF, ARCHBFD_FORMAT_ELF);
SAME(qbfd::Format::PE, ARCHBFD_FORMAT_PE);
SAME(qbfd::Format::COFF, ARCHBFD_FORMAT_COFF);
SAME(qbfd::Format::MachO, ARCHBFD_FORMAT_MACHO);
SAME(qbfd::Arch::Unknown, ARCHBFD_ARCH_UNKNOWN);
SAME(qbfd::Arch::X86, ARCHBFD_ARCH_X86);
SAME(qbfd::Arch::X86_64, ARCHBFD_ARCH_X86_64);
SAME(qbfd::Arch::ARM, ARCHBFD_ARCH_ARM);
SAME(qbfd::Arch::AArch64, ARCHBFD_ARCH_AARCH64);
SAME(qbfd::Arch::RISCV32, ARCHBFD_ARCH_RISCV32);
SAME(qbfd::Arch::RISCV64, ARCHBFD_ARCH_RISCV64);
SAME(qbfd::Arch::PowerPC, ARCHBFD_ARCH_POWERPC);
SAME(qbfd::Arch::PowerPC64, ARCHBFD_ARCH_POWERPC64);
SAME(qbfd::Arch::MIPS, ARCHBFD_ARCH_MIPS);
SAME(qbfd::Arch::MIPS64, ARCHBFD_ARCH_MIPS64);
SAME(qbfd::Arch::SPARC, ARCHBFD_ARCH_SPARC);
SAME(qbfd::Arch::SPARC64, ARCHBFD_ARCH_SPARC64);
SAME(qbfd::Arch::S390X, ARCHBFD_ARCH_S390X);
SAME(qbfd::Arch::LoongArch64, ARCHBFD_ARCH_LOONGARCH64);
SAME(qbfd::Arch::WebAssembly, ARCHBFD_ARCH_WEBASSEMBLY);
SAME(qbfd::Endian::Little, ARCHBFD_LITTLE_ENDIAN);
SAME(qbfd::Endian::Big, ARCHBFD_BIG_ENDIAN);
SAME(qbfd::FileType::Unknown, ARCHBFD_FILE_UNKNOWN);
SAME(qbfd::FileType::Relocatable, ARCHBFD_FILE_RELOCATABLE);
SAME(qbfd::FileType::Executable, ARCHBFD_FILE_EXECUTABLE);
SAME(qbfd::FileType::SharedObject, ARCHBFD_FILE_SHARED);
SAME(qbfd::FileType::Core, ARCHBFD_FILE_CORE);
SAME(qbfd::sec::Alloc, ARCHBFD_SEC_ALLOC);
SAME(qbfd::sec::Load, ARCHBFD_SEC_LOAD);
SAME(qbfd::sec::Code, ARCHBFD_SEC_CODE);
SAME(qbfd::sec::Data, ARCHBFD_SEC_DATA);
SAME(qbfd::sec::ReadOnly, ARCHBFD_SEC_READ_ONLY);
SAME(qbfd::sec::Writable, ARCHBFD_SEC_WRITABLE);
SAME(qbfd::sec::Exec, ARCHBFD_SEC_EXEC);
SAME(qbfd::sec::Bss, ARCHBFD_SEC_BSS);
SAME(qbfd::sec::Debug, ARCHBFD_SEC_DEBUG);
SAME(qbfd::sec::Reloc, ARCHBFD_SEC_RELOC);
SAME(qbfd::sec::Strings, ARCHBFD_SEC_STRINGS);
SAME(qbfd::sec::Note, ARCHBFD_SEC_NOTE);
SAME(qbfd::sec::Tls, ARCHBFD_SEC_TLS);
SAME(qbfd::sec::Discard, ARCHBFD_SEC_DISCARD);
SAME(qbfd::sym::Undefined, ARCHBFD_SYM_UNDEFINED);
SAME(qbfd::sym::Absolute, ARCHBFD_SYM_ABSOLUTE);
SAME(qbfd::sym::Hidden, ARCHBFD_SYM_HIDDEN);
SAME(qbfd::sym::Exported, ARCHBFD_SYM_EXPORTED);
SAME(qbfd::sym::Imported, ARCHBFD_SYM_IMPORTED);
SAME(qbfd::sym::Synthetic, ARCHBFD_SYM_SYNTHETIC);
SAME(qbfd::sym::Debug, ARCHBFD_SYM_DEBUG);
SAME(qbfd::SymbolBinding::Local, ARCHBFD_BIND_LOCAL);
SAME(qbfd::SymbolBinding::Global, ARCHBFD_BIND_GLOBAL);
SAME(qbfd::SymbolBinding::Weak, ARCHBFD_BIND_WEAK);
SAME(qbfd::SymbolBinding::Unique, ARCHBFD_BIND_UNIQUE);
SAME(qbfd::SymbolKind::None, ARCHBFD_SYMBOL_NONE);
SAME(qbfd::SymbolKind::Object, ARCHBFD_SYMBOL_OBJECT);
SAME(qbfd::SymbolKind::Function, ARCHBFD_SYMBOL_FUNCTION);
SAME(qbfd::SymbolKind::Section, ARCHBFD_SYMBOL_SECTION);
SAME(qbfd::SymbolKind::File, ARCHBFD_SYMBOL_FILE);
SAME(qbfd::SymbolKind::Common, ARCHBFD_SYMBOL_COMMON);
SAME(qbfd::SymbolKind::Tls, ARCHBFD_SYMBOL_TLS);
SAME(qbfd::SymbolKind::Indirect, ARCHBFD_SYMBOL_INDIRECT);
SAME(qbfd::seg::Read, ARCHBFD_SEG_READ);
SAME(qbfd::seg::Write, ARCHBFD_SEG_WRITE);
SAME(qbfd::seg::Exec, ARCHBFD_SEG_EXEC);
#undef SAME

archbfd_status fail(archbfd_error* error, archbfd_status code,
                    const char* message) noexcept {
  if (error) {
    archbfd_error_clear(error);
    error->code = code;
    if (message) {
      auto n = std::min(std::strlen(message), sizeof(error->message) - 1);
      std::memcpy(error->message, message, n);
    }
  }
  return code;
}
archbfd_status fail(archbfd_error* error, const qbfd::Error& source) noexcept {
  using Code = qbfd::Error::Code;
  archbfd_status code = ARCHBFD_INTERNAL_ERROR;
  switch (source.code) {
    case Code::None: break; // An error alternative with None is still a failure.
    case Code::FileNotFound: code = ARCHBFD_NOT_FOUND; break;
    case Code::IoError: code = ARCHBFD_IO_ERROR; break;
    case Code::UnknownFormat: code = ARCHBFD_UNKNOWN_FORMAT; break;
    case Code::Ambiguous: code = ARCHBFD_AMBIGUOUS; break;
    case Code::Malformed: code = ARCHBFD_MALFORMED; break;
    case Code::Unsupported: code = ARCHBFD_UNSUPPORTED; break;
    case Code::OutOfRange: code = ARCHBFD_OUT_OF_RANGE; break;
  }
  return fail(error, code, source.message.c_str());
}
template <class F>
archbfd_status guard(archbfd_error* error, F&& f) noexcept {
  archbfd_error_clear(error);
  try {
    return f();
  } catch (const qbfd::detail::ParseFailure& e) {
    return fail(error, e.error);
  } catch (const std::bad_alloc&) {
    return fail(error, ARCHBFD_NO_MEMORY, "allocation failed");
  } catch (const std::length_error& e) {
    return fail(error, ARCHBFD_OUT_OF_RANGE, e.what());
  } catch (const std::filesystem::filesystem_error& e) {
    return fail(error, ARCHBFD_IO_ERROR, e.what());
  } catch (const std::exception& e) {
    return fail(error, ARCHBFD_INTERNAL_ERROR, e.what());
  } catch (...) {
    return fail(error, ARCHBFD_INTERNAL_ERROR, "unrecognized C++ exception");
  }
}
archbfd_status invalid(archbfd_error* e) {
  return fail(e, ARCHBFD_INVALID_ARGUMENT, "null argument or invalid selector");
}
archbfd_status range(archbfd_error* e) {
  return fail(e, ARCHBFD_OUT_OF_RANGE, "table index out of range");
}
archbfd_string view(const std::string& s) { return {s.data(), s.size()}; }
archbfd_bytes view(std::span<const uint8_t> s) { return {s.data(), s.size()}; }
std::vector<uint8_t> copy(const void* data, size_t size) {
  if (!size) return {};
  auto p = static_cast<const uint8_t*>(data);
  return {p, p + size};
}
archbfd_status finish(std::unique_ptr<qbfd::ObjectFile> file,
                      archbfd_object** out, archbfd_error* error) {
  auto result = std::make_unique<archbfd_object>();
  result->target = file->targetName();
  result->dependencies = file->neededLibraries();
  for (const auto& section : file->sections()) {
    auto entries = file->relocations(section);
    if (!entries) return fail(error, entries.error());
    result->relocations.push_back(std::move(*entries));
  }
  result->file = std::move(file);
  *out = result.release();
  return ARCHBFD_OK;
}
archbfd_status finish(qbfd::Expected<qbfd::Opened> opened,
                      archbfd_object** out, archbfd_error* error) {
  if (!opened) return fail(error, opened.error());
  return finish(std::move(opened->object), out, error);
}
archbfd_status finish(qbfd::Expected<qbfd::OpenedArchive> opened,
                      archbfd_archive** out, archbfd_error* error) {
  if (!opened) return fail(error, opened.error());
  auto result = std::make_unique<archbfd_archive>();
  result->opened = std::move(*opened);
  *out = result.release();
  return ARCHBFD_OK;
}
} // namespace

extern "C" {
archbfd_status archbfd_open_file(const char* path, const char* target,
                                archbfd_object** out, archbfd_error* error) {
  if (out) *out = nullptr;
  return guard(error, [&]() -> archbfd_status {
    if (!out || !path || !*path) return invalid(error);
    return finish(target ? qbfd::openAs(path, target) : qbfd::open(path), out, error);
  });
}
archbfd_status archbfd_open_memory(const void* data, size_t size, const char* target,
                                  archbfd_object** out, archbfd_error* error) {
  if (out) *out = nullptr;
  return guard(error, [&]() -> archbfd_status {
    if (!out || (!data && size)) return invalid(error);
    if (qbfd::macho::isFat({static_cast<const uint8_t*>(data), size}))
      return fail(error, ARCHBFD_UNSUPPORTED, "select a universal Mach-O slice explicitly");
    return finish(target ? qbfd::openAs(copy(data, size), target)
                         : qbfd::open(copy(data, size)), out, error);
  });
}
archbfd_status archbfd_open_macho_slice(const void* data, size_t size, size_t slice,
                                      archbfd_object** out, archbfd_error* error) {
  if (out) *out = nullptr;
  return guard(error, [&]() -> archbfd_status {
    if (!out || (!data && size)) return invalid(error);
    auto bytes = qbfd::Buffer::fromBytes(copy(data, size));
    auto file = qbfd::macho::openSlice(bytes->span(), slice);
    if (!file) return fail(error, file.error());
    (*file)->retainBuffer(bytes);
    return finish(std::move(*file), out, error);
  });
}
archbfd_status archbfd_macho_slice_count(const void* data, size_t size,
                                       size_t* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!out || (!data && size)) return invalid(error);
    auto table = qbfd::macho::slices({static_cast<const uint8_t*>(data), size});
    if (!table) return fail(error, table.error());
    *out = table->size(); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_macho_slice(const void* data, size_t size, size_t slot,
                                     archbfd_macho_slice* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!out || (!data && size)) return invalid(error);
    auto table = qbfd::macho::slices({static_cast<const uint8_t*>(data), size});
    if (!table) return fail(error, table.error());
    if (slot >= table->size()) return range(error);
    const auto& s = (*table)[slot];
    *out = {s.cpuType, s.cpuSubtype, s.alignmentPower, s.offset, s.size};
    return ARCHBFD_OK;
  });
}
void archbfd_close(archbfd_object* object) { delete object; }

archbfd_status archbfd_get_object_info(const archbfd_object* object,
                                     archbfd_object_info* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    const auto& f = *object->file;
    archbfd_object_info value{};
    value.format = uint32_t(f.format()); value.arch = uint32_t(f.arch());
    value.endian = uint32_t(f.endian()); value.file_type = uint32_t(f.fileType());
    value.address_bits = f.is64Bit() ? 64 : 32;
    value.entry_point = f.entryPoint(); value.image_base = f.imageBase();
    value.file_size = f.data().size(); value.section_count = f.sections().size();
    value.symbol_count = f.symbols().size();
    value.dynamic_symbol_count = f.dynamicSymbols().size();
    value.segment_count = f.segments().size();
    value.dependency_count = object->dependencies.size(); value.target = view(object->target);
    *out = value;
    return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_bytes(const archbfd_object* object,
                               archbfd_bytes* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    *out = view(object->file->data()); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_section(const archbfd_object* object, size_t slot,
                                 archbfd_section* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    const auto& sections = object->file->sections();
    if (slot >= sections.size()) return range(error);
    const auto& s = sections[slot];
    *out = {s.index, s.flags, s.nativeType, s.nativeFlags, s.vma, s.lma, s.size,
            s.fileOffset, s.fileSize, s.alignment, view(s.name)};
    return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_section_bytes(const archbfd_object* object, size_t slot,
                                       archbfd_bytes* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    if (slot >= object->file->sections().size()) return range(error);
    auto bytes = object->file->sectionContents(object->file->sections()[slot]);
    if (!bytes) return fail(error, bytes.error());
    *out = view(*bytes); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_symbol(const archbfd_object* object, uint32_t table,
                                size_t slot, archbfd_symbol* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out || table > ARCHBFD_DYNAMIC_SYMBOLS) return invalid(error);
    const auto& symbols = table == ARCHBFD_STATIC_SYMBOLS ? object->file->symbols()
                                                        : object->file->dynamicSymbols();
    if (slot >= symbols.size()) return range(error);
    const auto& s = symbols[slot];
    *out = {s.index, s.section.value_or(ARCHBFD_NO_INDEX), uint32_t(s.binding),
            uint32_t(s.kind), s.flags, s.nativeType, s.value, s.size, view(s.name)};
    return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_relocation_count(const archbfd_object* object, size_t section,
                                           size_t* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    if (section >= object->relocations.size()) return range(error);
    *out = object->relocations[section].size(); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_relocation(const archbfd_object* object, size_t section,
                                    size_t slot, archbfd_relocation* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    if (section >= object->relocations.size() || slot >= object->relocations[section].size())
      return range(error);
    const auto& r = object->relocations[section][slot];
    *out = {r.offset, r.nativeValue, r.addend, object->file->sections()[section].index,
            r.symbol.value_or(ARCHBFD_NO_INDEX), r.targetSection.value_or(ARCHBFD_NO_INDEX),
            r.nativeType, r.bitSize, r.pcRelative, r.hasAddend, r.dynamicSymbol,
            r.scattered, view(r.typeName)};
    return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_segment(const archbfd_object* object, size_t slot,
                                 archbfd_segment* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    if (slot >= object->file->segments().size()) return range(error);
    const auto& s = object->file->segments()[slot];
    *out = {s.flags, s.nativeType, s.vaddr, s.paddr, s.fileOffset, s.fileSize,
            s.memSize, s.alignment, view(s.name)};
    return ARCHBFD_OK;
  });
}
archbfd_status archbfd_get_dependency(const archbfd_object* object, size_t slot,
                                    archbfd_string* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    if (slot >= object->dependencies.size()) return range(error);
    *out = view(object->dependencies[slot]); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_bytes_at_address(const archbfd_object* object, uint64_t address,
                                      uint64_t size, archbfd_bytes* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!object || !out) return invalid(error);
    auto bytes = object->file->bytesAtAddress(address, size);
    if (!bytes) return fail(error, bytes.error());
    *out = view(*bytes); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_archive_open_file(const char* path, archbfd_archive** out,
                                       archbfd_error* error) {
  if (out) *out = nullptr;
  return guard(error, [&]() -> archbfd_status {
    if (!path || !*path || !out) return invalid(error);
    return finish(qbfd::openArchive(std::filesystem::path(path)), out, error);
  });
}
archbfd_status archbfd_archive_open_memory(const void* data, size_t size, const char* label,
                                         archbfd_archive** out, archbfd_error* error) {
  if (out) *out = nullptr;
  return guard(error, [&]() -> archbfd_status {
    if (!out || (!data && size)) return invalid(error);
    return finish(qbfd::openArchive(copy(data, size), label ? label : ""), out, error);
  });
}
void archbfd_archive_close(archbfd_archive* archive) { delete archive; }
archbfd_status archbfd_archive_member_count(const archbfd_archive* archive,
                                           size_t* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!archive || !out) return invalid(error);
    *out = archive->opened.archive->members().size(); return ARCHBFD_OK;
  });
}
archbfd_status archbfd_archive_get_member(const archbfd_archive* archive, size_t slot,
                                         archbfd_member* out, archbfd_error* error) {
  return guard(error, [&]() -> archbfd_status {
    if (!archive || !out) return invalid(error);
    const auto& members = archive->opened.archive->members();
    if (slot >= members.size()) return range(error);
    const auto& m = members[slot];
    *out = {m.headerOffset, m.offset, m.size, m.mtime, m.mode, m.uid, m.gid,
            m.special, view(m.name)};
    return ARCHBFD_OK;
  });
}
archbfd_status archbfd_archive_open_member(archbfd_archive* archive, size_t slot,
                                         archbfd_object** out, archbfd_error* error) {
  if (out) *out = nullptr;
  return guard(error, [&]() -> archbfd_status {
    if (!archive || !out) return invalid(error);
    const auto& members = archive->opened.archive->members();
    if (slot >= members.size()) return range(error);
    auto file = archive->opened.archive->openMember(members[slot]);
    if (!file) return fail(error, file.error());
    return finish(std::move(*file), out, error);
  });
}
} // extern "C"
