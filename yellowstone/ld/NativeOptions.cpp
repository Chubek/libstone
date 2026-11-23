// The native driver's bounded argv frontend. No unsupported option is forwarded
// or silently used as a reason to start another linker.
#include "Backend.hpp"
#include "internal.hpp"
#include <stdexcept>

namespace qld::detail {
namespace {
ToolchainOptions& config(LinkOptions& o, Backend kind) {
  return kind == Backend::PE ? o.pe : kind == Backend::MachO ? o.macho : o.elf;
}
uint64_t number(const std::string& text) {
  if (text.empty() || text[0] == '-') throw std::runtime_error("invalid address: " + text);
  size_t used = 0;
  auto result = std::stoull(text, &used, 0);
  if (used != text.size()) throw std::runtime_error("invalid address: " + text);
  return result;
}

void parseNativeArguments(LinkOptions& o, Backend kind, const std::vector<std::string>& args) {
  // GNU-style ELF library search is dynamic by default; --static/-Bstatic
  // switch it back to archives. Other formats retain their native defaults.
  bool dynamicLibraries =
      kind == Backend::ELF ? !o.staticLink : o.preferSharedLibraries;
  if (o.librariesDynamic.size() < o.libs.size())
    o.librariesDynamic.resize(o.libs.size(), dynamicLibraries);
  auto library = [&](std::string name) {
    o.libs.push_back(std::move(name));
    o.librariesDynamic.push_back(dynamicLibraries);
  };
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    const auto low = toolchain::lower(a);
    auto value = [&]() -> const std::string& {
      if (++i == args.size()) throw std::runtime_error("missing argument for " + a);
      return args[i];
    };
    if (a == "--") {
      o.inputs.insert(o.inputs.end(), args.begin() + i + 1, args.end()); break;
    }
    if (a == "--format" || a.starts_with("--format=")) {
      auto requested = namedBackend(a == "--format" ? value() : a.substr(9));
      if (requested != Backend::Auto && requested != kind)
        throw std::runtime_error("conflicting linker formats");
    } else if (a == "-o" || a == "--output") o.output = value();
    else if (a.starts_with("--output=")) o.output = a.substr(9);
    else if (a == "-e" || a == "--entry") { o.entry = value(); o.hasEntry = true; }
    else if (a.starts_with("--entry=")) { o.entry = a.substr(8); o.hasEntry = true; }
    else if (a == "-L" || a == "--library-path") o.libPaths.push_back(value());
    else if (a.starts_with("-L")) o.libPaths.push_back(a.substr(2));
    else if (a.starts_with("--library-path=")) o.libPaths.push_back(a.substr(15));
    else if (a == "-l" || a == "--library") library(value());
    else if (a.starts_with("-l")) library(a.substr(2));
    else if (a.starts_with("--library=")) library(a.substr(10));
    else if (a == "-Map" || a == "--Map") o.mapFile = value();
    else if (a.starts_with("-Map=") || a.starts_with("--Map=")) o.mapFile = a.substr(a.find('=') + 1);
    else if (a == "-T" || a == "--script") o.script = value();
    else if (a.starts_with("--script=")) o.script = a.substr(9);
    else if (a == "--lto-script" || a.starts_with("--lto-script=")) {
      if (!o.ltoScript.empty()) throw std::runtime_error("specify --lto-script only once");
      o.ltoScript = a == "--lto-script" ? value() : a.substr(13);
      if (o.ltoScript.empty()) throw std::runtime_error("--lto-script needs a file");
    } else if (a.starts_with("-T") && !a.starts_with("-Ttext") &&
               !a.starts_with("-Tdata") && !a.starts_with("-Tbss")) o.script = a.substr(2);
    else if (a == "--script-text" || a.starts_with("--script-text=")) {
      o.scriptText = a == "--script-text" ? value() : a.substr(14); o.scriptIsText = true;
    } else if (a == "--image-base" || a.starts_with("--image-base=")) {
      o.base = number(a == "--image-base" ? value() : a.substr(13)); o.hasBase = true;
    } else if (a == "-m" || a == "--emulation" || (kind == Backend::MachO && a == "-arch"))
      o.emulator = value();
    else if (a == "--static" || a == "-static") {
      o.staticLink = true;
      dynamicLibraries = false;
    } else if (a == "-Bstatic" || a == "-dn" || a == "-non_shared") {
      dynamicLibraries = false;
    } else if (a == "-Bdynamic" || a == "-dy" || a == "-call_shared") {
      dynamicLibraries = true;
      o.preferSharedLibraries = true;
    }
    else if (a == "--shared" || a == "-shared" || (kind == Backend::PE && low == "/dll") ||
              (kind == Backend::MachO && a == "-dylib")) o.mode = Mode::Shared;
    else if (a == "--pie" || a == "-pie") o.pie = true;
    else if (a == "--no-pie" || a == "-no-pie") o.pie = false;
    else if (a == "-r" || a == "--relocatable") o.mode = Mode::Relocatable;
    else if (a == "--gc-sections") o.gcSections = true;
    else if (a == "--strip-debug" || a == "-s") o.stripDebug = true;
    else if (a == "--allow-undefined") o.allowUndefined = true;
    else if (kind == Backend::ELF && (a == "--no-undefined" || a == "-zdefs"))
      o.noUndefined = true;
    else if (kind == Backend::ELF && (a == "-E" || a == "--export-dynamic"))
      o.exportDynamic = true;
    else if (kind == Backend::ELF && (a == "-init" || a == "--init")) o.initSymbol = value();
    else if (kind == Backend::ELF && (a == "-fini" || a == "--fini")) o.finiSymbol = value();
    else if (kind == Backend::ELF && (a == "--defsym" || a.starts_with("--defsym="))) {
      const std::string expr = a == "--defsym" ? value() : a.substr(9);
      const auto sep = expr.find('=');
      if (!sep || sep == std::string::npos) throw std::runtime_error("expected --defsym NAME=INTEGER");
      if (!o.absoluteSymbols.emplace(expr.substr(0, sep), number(expr.substr(sep + 1))).second)
        throw std::runtime_error("duplicate --defsym definition: " + expr);
    }
    else if (kind == Backend::ELF && (a == "--soname" || a == "-soname" || a == "-h"))
      o.soname = value();
    else if (kind == Backend::ELF && (a.starts_with("--soname=") || a.starts_with("-soname=")))
      o.soname = a.substr(a.find('=') + 1);
    else if (kind == Backend::ELF && (a == "--rpath" || a == "-rpath"))
      o.runpath = value();
    else if (kind == Backend::ELF && (a.starts_with("--rpath=") || a.starts_with("-rpath=")))
      o.runpath = a.substr(a.find('=') + 1);
    else if (kind == Backend::ELF && (a == "--dynamic-linker" || a == "-dynamic-linker" ||
                                      a == "-I"))
      o.dynamicLinker = value();
    else if (kind == Backend::ELF &&
             (a.starts_with("--dynamic-linker=") || a.starts_with("-dynamic-linker=")))
      o.dynamicLinker = a.substr(a.find('=') + 1);
    else if (kind == Backend::ELF && (a == "--hash-style" || a.starts_with("--hash-style="))) {
      auto style = a == "--hash-style" ? value() : a.substr(13);
      if (style == "sysv") o.hashStyle = LinkOptions::HashStyle::SysV;
      else if (style == "gnu") o.hashStyle = LinkOptions::HashStyle::GNU;
      else if (style == "both") o.hashStyle = LinkOptions::HashStyle::Both;
      else throw std::runtime_error("unsupported native ELF hash style: " + style);
    } else if (kind == Backend::ELF && a == "--enable-new-dtags") o.newDtags = true;
    else if (kind == Backend::ELF && a == "--disable-new-dtags") o.newDtags = false;
    else if (kind == Backend::ELF && a == "--as-needed") o.asNeeded = true;
    else if (kind == Backend::ELF && a == "--no-as-needed") o.asNeeded = false;
    else if (kind == Backend::ELF && a == "-z") {
      auto z = value();
      if (z == "now") o.bindNow = true;
      else if (z == "lazy") o.bindNow = false;
      else if (z == "relro") o.relro = true;
      else if (z == "norelro") o.relro = false;
      else if (z == "defs") o.noUndefined = true;
      else if (z == "undefs") o.noUndefined = false;
      else if (z == "noexecstack") {}
      else throw std::runtime_error("unsupported native ELF -z option: " + z);
    }
    else if (a == "--verbose" || a == "-v") o.verbose = true;
    else if (kind == Backend::PE && low.starts_with("/entry:")) {
      o.entry = a.substr(7); o.hasEntry = true;
    } else if (kind == Backend::PE && low.starts_with("/out:")) o.output = a.substr(5);
    else if (kind == Backend::PE && low.starts_with("/base:")) { o.base = number(a.substr(6)); o.hasBase = true; }
    else if (kind == Backend::PE && low.starts_with("/libpath:")) o.libPaths.push_back(a.substr(9));
    else if (kind == Backend::PE && low.starts_with("/machine:")) o.emulator = low.substr(9);
    else if (kind == Backend::PE && (low == "/nodefaultlib" || low == "/fixed" ||
             low == "/dynamicbase:no" || low == "/nxcompat" || low == "/subsystem:console")) {}
    else if (a.starts_with("-o") && a.size() > 2 && !a.starts_with("--") &&
             !a.starts_with("-output") && !a.starts_with("-oformat") &&
             !a.starts_with("-orphan") && !a.starts_with("-opt")) o.output = a.substr(2);
    else if ((!a.empty() && a[0] == '-') ||
             (kind == Backend::PE && a.starts_with('/') && !std::filesystem::exists(a)))
      throw std::runtime_error("unsupported native linker option: " + a);
    else o.inputs.push_back(a);
  }
  if (!o.ltoScript.empty() && (kind != Backend::ELF || o.mode != Mode::Exec || o.pie ||
                              !o.dynamicLinker.empty() || !o.soname.empty() ||
                              !o.runpath.empty() || o.exportDynamic))
    throw std::runtime_error("--lto-script requires a native static ELF executable link");
  if (kind != Backend::ELF && (o.mode == Mode::Shared || o.pie))
    throw std::runtime_error("native PE/Mach-O linker does not yet support shared/PIE images");
  if (o.mode == Mode::Relocatable && o.pie)
    throw std::runtime_error("native linker cannot combine relocatable and PIE output");
  if (o.mode == Mode::Shared && o.pie)
    throw std::runtime_error("native linker cannot combine shared and PIE output");
  if (o.staticLink && (o.mode == Mode::Shared || o.pie))
    throw std::runtime_error("native ELF cannot combine --static with shared/PIE output");
  o.preferSharedLibraries = dynamicLibraries;
  if (kind != Backend::ELF && (o.scriptIsText || !o.script.empty()))
    throw std::runtime_error("native PE/Mach-O linker does not support ELF linker scripts");
}
} // namespace

bool configureLinkerArguments(LinkOptions& o, Backend kind) {
  auto& c = config(o, kind);
  auto args = toolchain::expandArguments(c.arguments, kind == Backend::PE ?
      toolchain::Quoting::Windows : toolchain::Quoting::GNU);
  // Only explicit requests select a bridge. Never forward a native hook.
  bool external = !c.linker.empty(), lto = !o.ltoScript.empty();
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--") break;
    if (a == "--dynamic" || a == "--elf-linker" || a.starts_with("--elf-linker=") ||
        a == "--pe-linker" || a.starts_with("--pe-linker=") ||
        a == "--macho-linker" || a.starts_with("--macho-linker=")) external = true;
    if (a == "--lto-script" || a.starts_with("--lto-script=")) lto = true;
    i += toolchain::optionArity(a);
  }
  if (external) {
    if (lto) throw std::runtime_error("--lto-script cannot be used with an external linker");
    // Use the inspected arguments, not a second read of a response file.
    c.arguments = std::move(args);
  } else {
    parseNativeArguments(o, kind, args);
  }
  return external;
}
} // namespace qld::detail
