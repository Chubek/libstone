#include "xbind_discovery.hpp"

#include <stdexcept>

#if XBIND_HAVE_DISCOVERY
#include "xbind_codegen.hpp"
#include "xbind_json.hpp"

#include <qBFD.hpp>

#include <algorithm>
#include <sstream>
#include <tuple>
#endif

namespace xbind::discovery {

bool available() noexcept { return XBIND_HAVE_DISCOVERY != 0; }

#if XBIND_HAVE_DISCOVERY
namespace {
using detail::json_quote;

qbfd::Opened open_image(const std::filesystem::path& path, std::string_view arch) {
  auto buffer = qbfd::Buffer::fromFile(path);
  if (!buffer) throw std::runtime_error(buffer.error().message);
  qbfd::Arch selected = qbfd::Arch::Unknown;
  if (!arch.empty()) {
    // All currently known architecture enum values. Accept canonical qBFD names.
    for (unsigned n = 1; n <= static_cast<unsigned>(qbfd::Arch::WebAssembly); ++n) {
      auto candidate = static_cast<qbfd::Arch>(n);
      if (qbfd::toString(candidate) == arch) selected = candidate;
    }
    if (selected == qbfd::Arch::Unknown)
      throw std::runtime_error("unknown architecture: " + std::string(arch));
  }
  const auto data = (*buffer)->span();
  auto opened = [&]() -> qbfd::Expected<qbfd::Opened> {
    if (qbfd::macho::isFat(data)) {
      if (selected == qbfd::Arch::Unknown)
        throw std::runtime_error("universal Mach-O requires an ARCH");
      return qbfd::openMachOSlice(std::vector<uint8_t>(data.begin(), data.end()), selected, path);
    }
    return qbfd::openImpl(*buffer, path, nullptr);
  }();
  if (!opened) throw std::runtime_error(opened.error().message);
  if (selected != qbfd::Arch::Unknown && opened->object->arch() != selected)
    throw std::runtime_error("object architecture does not match " + std::string(arch));
  return std::move(*opened);
}

std::string_view kind_name(qbfd::ImageKind kind) {
  switch (kind) {
    case qbfd::ImageKind::Unknown: return "unknown";
    case qbfd::ImageKind::Relocatable: return "relocatable";
    case qbfd::ImageKind::Executable: return "executable";
    case qbfd::ImageKind::SharedLibrary: return "shared-library";
    case qbfd::ImageKind::Core: return "core";
    case qbfd::ImageKind::PositionIndependentExecutable: return "pie";
  }
  return "unknown";
}

void image_json(std::ostream& out, const qbfd::ImageInfo& info) {
  out << "{\"format\":" << json_quote(qbfd::toString(info.format))
      << ",\"arch\":" << json_quote(qbfd::toString(info.arch))
      << ",\"endian\":" << json_quote(qbfd::toString(info.endian))
      << ",\"address_bits\":" << unsigned(info.addressBits)
      << ",\"kind\":" << json_quote(kind_name(info.kind))
      << ",\"abi\":" << json_quote(qbfd::abi::toString(info.abi))
      << ",\"image_base\":" << info.imageBase
      << ",\"entry_point\":" << info.entryPoint << '}';
}

void symbol_json(std::ostream& out, const qbfd::LinkSymbol& s) {
  out << "{\"name\":" << json_quote(s.name)
      << ",\"library\":" << json_quote(s.library)
      << ",\"target\":" << json_quote(s.target)
      << ",\"version\":" << json_quote(s.version)
      << ",\"address\":";
  if (s.address) out << *s.address; else out << "null";
  out << ",\"ordinal\":";
  if (s.ordinal) out << *s.ordinal; else out << "null";
  out << ",\"weak\":" << (s.weak ? "true" : "false")
      << ",\"default_version\":" << (s.defaultVersion ? "true" : "false")
      << ",\"forwarded\":" << (s.forwarded ? "true" : "false") << '}';
}

auto symbol_key(const qbfd::LinkSymbol& s) {
  return std::tie(s.name, s.version, s.ordinal, s.library, s.target, s.address,
                  s.weak, s.defaultVersion, s.forwarded);
}

void symbols_json(std::ostream& out, std::vector<qbfd::LinkSymbol> symbols) {
  std::sort(symbols.begin(), symbols.end(), [](const auto& a, const auto& b) {
    return symbol_key(a) < symbol_key(b);
  });
  out << '[';
  bool first = true;
  for (const auto& s : symbols) {
    if (!first) out << ',';
    first = false;
    symbol_json(out, s);
  }
  out << ']';
}
} // namespace

std::string inventory(const std::filesystem::path& path, std::string_view arch) {
  const auto opened = open_image(path, arch);
  const auto& object = *opened.object;
  std::ostringstream out;
  out << "{\"inventory_version\":1,\"image\":";
  image_json(out, qbfd::imageInfo(object));
  auto needed = object.neededLibraries();
  // Preserve library ordering (it can be relevant to link resolution).
  out << ",\"needed\":[";
  for (size_t i = 0; i < needed.size(); ++i) {
    if (i) out << ',';
    out << json_quote(needed[i]);
  }
  out << "],\"imports\":";
  symbols_json(out, qbfd::imports(object));
  out << ",\"exports\":";
  symbols_json(out, qbfd::exports(object));
  return out.str() + "}\n";
}

std::string bind(const std::filesystem::path& path, const qamrpp::Manifest& manifest,
                 std::string_view arch) {
  if (manifest.functions.empty() || manifest.headers.empty())
    throw std::runtime_error("binary binding requires manifest declarations and headers");
  const auto opened = open_image(path, arch);
  const auto& object = *opened.object;
  const auto info = qbfd::imageInfo(object);
  if (info.kind != qbfd::ImageKind::SharedLibrary)
    throw std::runtime_error("binary binding requires a shared-library image");
  const auto exports = qbfd::exports(object);
  std::ostringstream out;
  out << "{\"binding_version\":1,\"signature_status\":\"declared-unchecked\",\"image\":";
  image_json(out, info);
  auto spec = codegen::dump_ir(manifest);
  spec.pop_back(); // Remove its terminating newline before embedding.
  out << ",\"manifest\":" << spec << ",\"bindings\":[";
  bool first = true;
  for (const auto& f : manifest.functions) {
    const auto raw_name = f.options.find("symbol");
    const auto version = f.options.find("symbol_version");
    const auto ordinal = f.options.find("symbol_ordinal");
    const auto name = raw_name != f.options.end() ? raw_name->second :
                      (info.format == qbfd::Format::MachO ? "_" + f.name : f.name);
    if (version != f.options.end() && info.format != qbfd::Format::ELF)
      throw std::runtime_error("symbol_version requires ELF: " + f.name);
    if (ordinal != f.options.end() && info.format != qbfd::Format::PE)
      throw std::runtime_error("symbol_ordinal requires PE: " + f.name);
    const auto selector = ordinal != f.options.end() ? "#" + ordinal->second :
                          version != f.options.end() ? name + "@" + version->second : name;
    const qbfd::LinkSymbol* match = nullptr;
    for (const auto& s : exports) {
      if (ordinal != f.options.end()) {
        if (!s.ordinal || std::to_string(*s.ordinal) != ordinal->second) continue;
      } else {
        if (s.name != name) continue;
        if (version != f.options.end() ? s.version != version->second :
            !s.version.empty() && !s.defaultVersion) continue;
      }
      if (match) throw std::runtime_error("ambiguous export: " + selector);
      match = &s;
    }
    if (!match) throw std::runtime_error("no matching export: " + selector);
    if (!first) out << ',';
    first = false;
    out << "{\"declaration\":" << json_quote(f.name) << ",\"export\":";
    symbol_json(out, *match);
    out << '}';
  }
  return out.str() + "]}\n";
}
#else
std::string inventory(const std::filesystem::path&, std::string_view) {
  throw std::runtime_error("binary discovery was not built (requires qBFD)");
}
std::string bind(const std::filesystem::path&, const qamrpp::Manifest&, std::string_view) {
  throw std::runtime_error("binary discovery was not built (requires qBFD)");
}
#endif
}
