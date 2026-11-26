#include "dynload.hpp"

#include <options.hpp>

#include <cstdint>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{'h', "help", false, "", "show this help"};
constexpr OptionSpec kLibraryPath{'L', "library-path", true, "dir", "add a library search directory"};
constexpr OptionSpec kDefaultPaths{0, "default-paths", false, "", "add platform library-path environment variables"};
constexpr OptionSpec kSelf{0, "self", false, "", "open the current process instead of a path"};
constexpr OptionSpec kInspectOnly{0, "inspect-only", false, "", "parse with qBFD without native loading"};
constexpr OptionSpec kRequireInspect{0, "require-inspect", false, "", "fail if qBFD cannot inspect the file"};
constexpr OptionSpec kLazy{0, "lazy", false, "", "use lazy binding"};
constexpr OptionSpec kNow{0, "now", false, "", "use immediate binding (default)"};
constexpr OptionSpec kLocal{0, "local", false, "", "keep symbols local to the handle (default)"};
constexpr OptionSpec kGlobal{0, "global", false, "", "make symbols globally visible to later loads"};
constexpr OptionSpec kNoDelete{0, "nodelete", false, "", "ask the host loader not to unload the image"};
constexpr OptionSpec kNoLoad{0, "noload", false, "", "look up an already-loaded image"};
constexpr OptionSpec kDeepBind{0, "deepbind", false, "", "prefer the image's own definitions where supported"};
constexpr OptionSpec kInspect{'i', "inspect", false, "", "print qBFD identity metadata"};
constexpr OptionSpec kNeeded{'n', "needed", false, "", "print shared-library dependencies"};
constexpr OptionSpec kExports{0, "exports", false, "", "print normalized qBFD exports"};
constexpr OptionSpec kImports{0, "imports", false, "", "print normalized qBFD imports"};
constexpr OptionSpec kManifest{0, "manifest", false, "", "emit an offline qBFD JSON manifest"};
constexpr OptionSpec kSymbols{'s', "symbols", false, "", "list dynamic symbols"};
constexpr OptionSpec kAllSymbols{'a', "all-symbols", false, "", "list static and dynamic symbols"};
constexpr OptionSpec kStaticSymbols{0, "static-symbols", false, "", "list the static symbol table"};
constexpr OptionSpec kSections{0, "sections", false, "", "list sections"};
constexpr OptionSpec kRelocs{0, "relocs", false, "", "list relocations"};
constexpr OptionSpec kResolve{'r', "resolve", true, "symbol", "resolve a symbol with the native loader"};
constexpr OptionSpec kResolveVersion{0, "resolve-version", true, "symbol@version", "resolve an ELF version with the native loader"};
constexpr OptionSpec kResolveOrdinal{0, "resolve-ordinal", true, "ordinal", "resolve a PE ordinal with the native loader"};
constexpr OptionSpec kPromote{'p', "promote", false, "", "promote loadable symbols into the dynload environment"};
constexpr OptionSpec kEnvSymbol{0, "env-symbol", true, "symbol", "resolve a symbol from the dynload environment"};
constexpr OptionSpec kAddress{0, "address", true, "addr", "find the qBFD symbol covering an address"};
constexpr OptionSpec kRuntimeAddress{0, "runtime-address", true, "linked-va", "translate a linked VA into a loaded image address"};

std::string hex(uint64_t value, unsigned width = 0) {
  std::ostringstream out;
  out << "0x" << std::hex << std::setfill('0');
  if (width) out << std::setw(width);
  out << value;
  return out.str();
}

std::string ptr(void* value) {
  return hex(reinterpret_cast<std::uintptr_t>(value), sizeof(void*) * 2);
}

bool parseNumber(const std::string& text, uint64_t& out) {
  try {
    size_t used = 0;
    out = std::stoull(text, &used, 0);
    return used == text.size();
  } catch (...) {
    return false;
  }
}

uint32_t openFlags(const qobj::Options& opts) {
  uint32_t flags = 0;
  if (opts.has("lazy")) flags |= TDYN_OPEN_LAZY;
  if (opts.has("now")) flags |= TDYN_OPEN_NOW;
  if (opts.has("local")) flags |= TDYN_OPEN_LOCAL;
  if (opts.has("global")) flags |= TDYN_OPEN_GLOBAL;
  if (opts.has("nodelete")) flags |= TDYN_OPEN_NODELETE;
  if (opts.has("noload")) flags |= TDYN_OPEN_NOLOAD;
  if (opts.has("deepbind")) flags |= TDYN_OPEN_DEEPBIND;
  if (opts.has("require-inspect")) flags |= TDYN_OPEN_REQUIRE_INSPECT;
  if (opts.has("inspect-only") || opts.has(kManifest)) flags |= TDYN_OPEN_INSPECT_ONLY;
  return flags;
}

tdyn_symbol_table_t selectedTable(const qobj::Options& opts) {
  if (opts.has("all-symbols")) return TDYN_SYMBOL_TABLE_ANY;
  if (opts.has("static-symbols")) return TDYN_SYMBOL_TABLE_STATIC;
  return TDYN_SYMBOL_TABLE_DYNAMIC;
}

const char* compatibility(tdyn_library_t* lib) {
  switch (tdyn_library_native_compatibility(lib)) {
    case TDYN_NATIVE_COMPAT_COMPATIBLE: return "compatible";
    case TDYN_NATIVE_COMPAT_INCOMPATIBLE: return "incompatible";
    default: return "unknown";
  }
}

void printInspect(tdyn_library_t* lib) {
  std::cout << "path: " << (tdyn_library_path(lib) ? tdyn_library_path(lib) : "") << '\n';
  std::cout << "native-loaded: " << (tdyn_library_is_native_loaded(lib) ? "yes" : "no") << '\n';
  std::cout << "native-compatibility: " << compatibility(lib) << '\n';
  if (!tdyn_library_is_inspected(lib)) {
    std::cout << "qBFD: unavailable";
    const char* err = tdyn_library_inspection_error(lib);
    if (err && *err) std::cout << " (" << err << ')';
    std::cout << '\n';
    return;
  }
  std::cout << "format: " << tdyn_library_format_name(lib) << '\n';
  std::cout << "arch: " << tdyn_library_arch_name(lib) << '\n';
  std::cout << "endian: " << tdyn_library_endian_name(lib) << '\n';
  std::cout << "file-type: " << tdyn_library_file_type_name(lib) << '\n';
  std::cout << "image-kind: " << tdyn_library_image_kind_name(lib) << '\n';
  std::cout << "address-bits: " << tdyn_library_address_bits(lib) << '\n';
  std::cout << "abi: " << tdyn_library_abi_name(lib) << '\n';
  std::cout << "entry: " << hex(tdyn_library_entry_point(lib)) << '\n';
  std::cout << "image-base: " << hex(tdyn_library_image_base(lib)) << '\n';
}

void printNeeded(tdyn_library_t* lib) {
  if (!tdyn_library_is_inspected(lib)) {
    std::cout << "needed libraries: unavailable (not inspected)\n";
    return;
  }
  size_t count = tdyn_library_needed_count(lib);
  std::cout << "needed libraries:" << '\n';
  for (size_t i = 0; i < count; ++i) std::cout << "  " << tdyn_library_needed(lib, i) << '\n';
}

void printSymbols(tdyn_library_t* lib, tdyn_symbol_table_t table) {
  size_t count = tdyn_library_symbol_count(lib, table);
  std::cout << "symbols:" << '\n';
  for (size_t i = 0; i < count; ++i) {
    tdyn_symbol_info_t s{};
    if (tdyn_library_symbol_info(lib, table, i, &s) != TDYN_OK) continue;
    char tableChar = s.table == TDYN_SYMBOL_TABLE_DYNAMIC ? 'D' : 'S';
    std::cout << "  " << tableChar << ' ' << std::setw(18) << hex(s.value)
              << " size=" << std::dec << s.size
              << " bind=" << tdyn_symbol_binding_name(s.binding)
              << " kind=" << tdyn_symbol_kind_name(s.kind);
    if (s.undefined) std::cout << " undefined";
    if (s.exported) std::cout << " exported";
    if (s.imported) std::cout << " imported";
    if (s.hidden) std::cout << " hidden";
    std::cout << "  " << (s.name ? s.name : "") << '\n';
  }
}

void printSections(tdyn_library_t* lib) {
  std::cout << "sections:" << '\n';
  for (size_t i = 0, n = tdyn_library_section_count(lib); i < n; ++i) {
    tdyn_section_info_t s{};
    if (tdyn_library_section_info(lib, i, &s) != TDYN_OK) continue;
    std::cout << "  [" << s.index << "] " << (s.name ? s.name : "")
              << " vma=" << hex(s.vma) << " size=" << hex(s.size)
              << " file=" << hex(s.file_offset) << "+" << hex(s.file_size)
              << " flags=" << hex(s.flags) << '\n';
  }
}

void printRelocs(tdyn_library_t* lib) {
  std::cout << "relocations:" << '\n';
  for (size_t i = 0, n = tdyn_library_relocation_count(lib); i < n; ++i) {
    tdyn_relocation_info_t r{};
    if (tdyn_library_relocation_info(lib, i, &r) != TDYN_OK) continue;
    std::cout << "  " << hex(r.offset) << " "
              << (r.type_name && *r.type_name ? r.type_name : hex(r.native_type));
    if (r.has_symbol) std::cout << " " << (r.symbol_name ? r.symbol_name : "<symbol>");
    if (r.addend) std::cout << " addend=" << r.addend;
    if (r.pc_relative) std::cout << " pc-relative";
    std::cout << '\n';
  }
}

int failWithLast(const std::string& what) {
  std::cerr << what << ": " << tdyn_last_error() << '\n';
  return 1;
}

int printLinkSymbols(tdyn_library_t* lib, tdyn_link_symbol_list_t list) {
  size_t count = 0;
  const char* title = list == TDYN_LINK_EXPORTS ? "exports" : "imports";
  if (tdyn_library_link_symbol_count(lib, list, &count) != TDYN_OK)
    return failWithLast(title);
  std::cout << title << ":\n";
  for (size_t i = 0; i < count; ++i) {
    tdyn_link_symbol_info_t symbol{};
    if (tdyn_library_link_symbol_info(lib, list, i, &symbol) != TDYN_OK)
      return failWithLast(title);
    std::cout << "  " << (*symbol.name ? symbol.name : "<unnamed>");
    if (*symbol.version) std::cout << (symbol.default_version ? "@@" : "@") << symbol.version;
    if (symbol.has_ordinal) std::cout << " ordinal=" << symbol.ordinal;
    if (symbol.has_address) std::cout << " va=" << hex(symbol.address);
    if (*symbol.library) std::cout << " from=" << symbol.library;
    if (symbol.forwarded) {
      std::cout << " forwarded";
      if (*symbol.target) std::cout << '=' << symbol.target;
    }
    if (symbol.weak) std::cout << " weak";
    std::cout << '\n';
  }
  return 0;
}

// Binary metadata strings are byte strings, not necessarily UTF-8. Escape
// non-ASCII bytes as their corresponding U+00xx values to keep JSON valid.
std::string jsonBytes(std::string_view text) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result = "\"";
  for (unsigned char ch : text) {
    if (ch == '"' || ch == '\\') {
      result += '\\';
      result += static_cast<char>(ch);
    } else if (ch < 0x20 || ch >= 0x7f) {
      result += "\\u00";
      result += digits[ch >> 4];
      result += digits[ch & 15];
    } else {
      result += static_cast<char>(ch);
    }
  }
  result += '"';
  return result;
}

int printManifest(tdyn_library_t* lib) {
  std::ostringstream out;
  out << "{\n  \"schema\": \"tdynload-inspection-v1\",\n"
      << "  \"path\": " << jsonBytes(tdyn_library_path(lib)) << ",\n"
      << "  \"native_loaded\": false,\n"
      << "  \"native_compatibility\": " << jsonBytes(compatibility(lib)) << ",\n"
      << "  \"format\": " << jsonBytes(tdyn_library_format_name(lib)) << ",\n"
      << "  \"architecture\": " << jsonBytes(tdyn_library_arch_name(lib)) << ",\n"
      << "  \"abi\": " << jsonBytes(tdyn_library_abi_name(lib)) << ",\n"
      << "  \"endian\": " << jsonBytes(tdyn_library_endian_name(lib)) << ",\n"
      << "  \"file_type\": " << jsonBytes(tdyn_library_file_type_name(lib)) << ",\n"
      << "  \"image_kind\": " << jsonBytes(tdyn_library_image_kind_name(lib)) << ",\n"
      << "  \"address_bits\": " << tdyn_library_address_bits(lib) << ",\n"
      << "  \"image_base\": " << jsonBytes(hex(tdyn_library_image_base(lib))) << ",\n"
      << "  \"entry_point\": " << jsonBytes(hex(tdyn_library_entry_point(lib))) << ",\n"
      << "  \"needed\": [";
  for (size_t i = 0, n = tdyn_library_needed_count(lib); i < n; ++i)
    out << (i ? ", " : "") << jsonBytes(tdyn_library_needed(lib, i));
  out << ']';
  for (auto list : {TDYN_LINK_IMPORTS, TDYN_LINK_EXPORTS}) {
    const char* title = list == TDYN_LINK_EXPORTS ? "exports" : "imports";
    size_t count = 0;
    if (tdyn_library_link_symbol_count(lib, list, &count) != TDYN_OK)
      return failWithLast(title);
    out << ",\n  \"" << title << "\": [";
    for (size_t i = 0; i < count; ++i) {
      tdyn_link_symbol_info_t s{};
      if (tdyn_library_link_symbol_info(lib, list, i, &s) != TDYN_OK)
        return failWithLast(title);
      out << (i ? ",\n" : "\n") << "    {\"name\": " << jsonBytes(s.name)
          << ", \"library\": " << jsonBytes(s.library)
          << ", \"target\": " << jsonBytes(s.target)
          << ", \"version\": " << jsonBytes(s.version)
          << ", \"ordinal\": " << (s.has_ordinal ? std::to_string(s.ordinal) : "null")
          << ", \"address\": " << (s.has_address ? jsonBytes(hex(s.address)) : "null")
          << ", \"default_version\": " << (s.default_version ? "true" : "false")
          << ", \"forwarded\": " << (s.forwarded ? "true" : "false")
          << ", \"weak\": " << (s.weak ? "true" : "false") << '}';
    }
    out << (count ? "\n  ]" : "]");
  }
  std::cout << out.str() << "\n}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  qobj::Options opts("tdynload", "[options] <library> [symbol ...]",
                     {kHelp,
                      kLibraryPath,
                      kDefaultPaths,
                      kSelf,
                      kInspectOnly,
                      kRequireInspect,
                      kLazy,
                      kNow,
                      kLocal,
                      kGlobal,
                      kNoDelete,
                      kNoLoad,
                      kDeepBind,
                      kInspect,
                      kNeeded,
                      kExports,
                      kImports,
                      kManifest,
                      kSymbols,
                      kAllSymbols,
                      kStaticSymbols,
                      kSections,
                      kRelocs,
                      kResolve,
                      kResolveVersion,
                      kResolveOrdinal,
                      kPromote,
                      kEnvSymbol,
                      kAddress,
                      kRuntimeAddress});
  if (!opts.parse(argc, argv)) {
    std::cerr << opts.error() << '\n' << opts.usage();
    return 2;
  }
  if (opts.has(kHelp)) {
    std::cout << opts.usage();
    return 0;
  }

  const auto& positional = opts.positional();
  bool useSelf = opts.has("self");
  if (!useSelf && positional.empty()) {
    std::cerr << opts.usage();
    return 2;
  }
  const bool nativeAction = opts.has(kResolve) || opts.has(kResolveVersion) ||
                           opts.has(kResolveOrdinal) || opts.has(kPromote) ||
                           opts.has(kEnvSymbol) || opts.has(kRuntimeAddress) ||
                           positional.size() > (useSelf ? 0u : 1u);
  if ((opts.has(kInspectOnly) || opts.has(kManifest)) && (nativeAction || useSelf)) {
    std::cerr << "offline inspection cannot perform native actions or open self\n";
    return 2;
  }

  tdyn_env_t* env = nullptr;
  if (tdyn_env_create(&env) != TDYN_OK) return failWithLast("create environment");
  std::unique_ptr<tdyn_env_t, decltype(&tdyn_env_destroy)> envGuard(env, tdyn_env_destroy);

  for (const auto& dir : opts.valueList(kLibraryPath))
    if (tdyn_env_add_search_path(env, dir.c_str()) != TDYN_OK)
      return failWithLast("add search path");
  if (opts.has("default-paths") && tdyn_env_add_default_search_paths(env) != TDYN_OK)
    return failWithLast("add default paths");

  tdyn_library_t* lib = nullptr;
  uint32_t flags = openFlags(opts);
  tdyn_status_t openStatus = useSelf
                                 ? tdyn_env_open_self(env, flags, &lib)
                                 : tdyn_env_open(env, positional[0].c_str(), flags, &lib);
  if (openStatus != TDYN_OK) return failWithLast("open");
  if (opts.has(kManifest)) return printManifest(lib);

  if (opts.has(kPromote) && tdyn_env_promote(env, lib, selectedTable(opts)) != TDYN_OK)
    return failWithLast("promote");

  bool anyAction = opts.has(kInspect) || opts.has(kNeeded) || opts.has(kSymbols) ||
                   opts.has(kAllSymbols) || opts.has(kStaticSymbols) ||
                   opts.has(kSections) || opts.has(kRelocs) || opts.has(kResolve) ||
                   opts.has(kExports) || opts.has(kImports) ||
                   opts.has(kResolveVersion) || opts.has(kResolveOrdinal) ||
                   opts.has(kEnvSymbol) || opts.has(kAddress) || opts.has(kRuntimeAddress);
  if (!anyAction && positional.size() <= (useSelf ? 0u : 1u)) {
    printInspect(lib);
    printNeeded(lib);
    return 0;
  }

  if (opts.has(kInspect)) printInspect(lib);
  if (opts.has(kNeeded)) printNeeded(lib);
  if (opts.has(kExports))
    if (int rc = printLinkSymbols(lib, TDYN_LINK_EXPORTS)) return rc;
  if (opts.has(kImports))
    if (int rc = printLinkSymbols(lib, TDYN_LINK_IMPORTS)) return rc;
  if (opts.has(kSymbols) || opts.has(kAllSymbols) || opts.has(kStaticSymbols))
    printSymbols(lib, selectedTable(opts));
  if (opts.has(kSections)) printSections(lib);
  if (opts.has(kRelocs)) printRelocs(lib);

  std::vector<std::string> resolves = opts.valueList(kResolve);
  size_t firstSymbol = useSelf ? 0 : 1;
  for (size_t i = firstSymbol; i < positional.size(); ++i) resolves.push_back(positional[i]);
  for (const auto& name : resolves) {
    void* address = nullptr;
    if (tdyn_library_symbol(lib, name.c_str(), &address) != TDYN_OK)
      return failWithLast("resolve " + name);
    std::cout << name << " = " << ptr(address) << '\n';
  }

  for (const auto& key : opts.valueList(kResolveVersion)) {
    const auto separator = key.find('@');
    if (separator == 0 || separator == std::string::npos || separator + 1 == key.size() ||
        key.find('@', separator + 1) != std::string::npos) {
      std::cerr << "expected symbol@version, got: " << key << '\n';
      return 2;
    }
    void* address = nullptr;
    if (tdyn_library_symbol_version(lib, key.substr(0, separator).c_str(),
                                    key.c_str() + separator + 1, &address) != TDYN_OK)
      return failWithLast("resolve version " + key);
    std::cout << key << " = " << ptr(address) << '\n';
  }

  for (const auto& text : opts.valueList(kResolveOrdinal)) {
    uint32_t ordinal = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), ordinal);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        ordinal == 0 || ordinal > UINT16_MAX) {
      std::cerr << "invalid decimal ordinal (expected 1..65535): " << text << '\n';
      return 2;
    }
    void* address = nullptr;
    if (tdyn_library_symbol_ordinal(lib, ordinal, &address) != TDYN_OK)
      return failWithLast("resolve ordinal " + text);
    std::cout << '#' << ordinal << " = " << ptr(address) << '\n';
  }

  for (const auto& name : opts.valueList(kEnvSymbol)) {
    void* address = nullptr;
    tdyn_library_t* owner = nullptr;
    if (tdyn_env_symbol(env, name.c_str(), &address, &owner) != TDYN_OK)
      return failWithLast("environment resolve " + name);
    std::cout << name << " = " << ptr(address);
    if (owner && tdyn_library_path(owner)) std::cout << " from " << tdyn_library_path(owner);
    std::cout << '\n';
  }

  for (const auto& text : opts.valueList(kRuntimeAddress)) {
    uint64_t address = 0, runtime = 0;
    if (!parseNumber(text, address) || text.starts_with('-')) {
      std::cerr << "invalid linked address: " << text << '\n';
      return 2;
    }
    if (tdyn_library_runtime_address(lib, address, &runtime) != TDYN_OK)
      return failWithLast("runtime address " + text);
    std::cout << text << " -> " << hex(runtime) << " (runtime address)\n";
  }

  for (const auto& text : opts.valueList(kAddress)) {
    uint64_t address = 0;
    if (!parseNumber(text, address)) {
      std::cerr << "invalid address: " << text << '\n';
      return 2;
    }
    tdyn_symbol_info_t info{};
    if (tdyn_library_symbol_at(lib, address, selectedTable(opts), &info) != TDYN_OK)
      return failWithLast("address lookup " + text);
    std::cout << text << " -> " << (info.name ? info.name : "<anonymous>")
              << " + " << hex(address - info.value) << '\n';
  }
  return 0;
}
