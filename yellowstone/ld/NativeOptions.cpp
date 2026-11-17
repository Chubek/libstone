// The native driver's bounded argv frontend. No unsupported option is forwarded
// or silently used as a reason to start another linker.
#include "Backend.hpp"
#include "internal.hpp"
#include <stdexcept>

namespace qld::detail {
namespace {
const ToolchainOptions& config(const LinkOptions& o, Backend kind) {
  return kind == Backend::PE ? o.pe : kind == Backend::MachO ? o.macho : o.elf;
}
uint64_t number(const std::string& text) {
  if (text.empty() || text[0] == '-') throw std::runtime_error("invalid address: " + text);
  size_t used = 0;
  auto result = std::stoull(text, &used, 0);
  if (used != text.size()) throw std::runtime_error("invalid address: " + text);
  return result;
}
}

bool externalBackendRequested(const LinkOptions& o, Backend kind) {
  const auto& c = config(o, kind);
  if (!c.linker.empty()) return true;
  // Only explicit requests select the compatibility bridges.
  auto args = toolchain::expandArguments(c.arguments, kind == Backend::PE ?
      toolchain::Quoting::Windows : toolchain::Quoting::GNU);
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if (a == "--") break;
    if (a == "--dynamic" || a == "--elf-linker" || a.starts_with("--elf-linker=") ||
        a == "--pe-linker" || a.starts_with("--pe-linker=") ||
        a == "--macho-linker" || a.starts_with("--macho-linker=")) return true;
    i += toolchain::optionArity(a);
  }
  return false;
}

void parseNativeArguments(LinkOptions& o, Backend kind) {
  const auto args = toolchain::expandArguments(config(o, kind).arguments,
      kind == Backend::PE ? toolchain::Quoting::Windows : toolchain::Quoting::GNU);
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
    else if (a == "-l" || a == "--library") o.libs.push_back(value());
    else if (a.starts_with("-l")) o.libs.push_back(a.substr(2));
    else if (a.starts_with("--library=")) o.libs.push_back(a.substr(10));
    else if (a == "-Map" || a == "--Map") o.mapFile = value();
    else if (a.starts_with("-Map=") || a.starts_with("--Map=")) o.mapFile = a.substr(a.find('=') + 1);
    else if (a == "-T" || a == "--script") o.script = value();
    else if (a.starts_with("--script=")) o.script = a.substr(9);
    else if (a.starts_with("-T") && !a.starts_with("-Ttext") &&
             !a.starts_with("-Tdata") && !a.starts_with("-Tbss")) o.script = a.substr(2);
    else if (a == "--script-text" || a.starts_with("--script-text=")) {
      o.scriptText = a == "--script-text" ? value() : a.substr(14); o.scriptIsText = true;
    } else if (a == "--image-base" || a.starts_with("--image-base=")) {
      o.base = number(a == "--image-base" ? value() : a.substr(13)); o.hasBase = true;
    } else if (a == "-m" || a == "--emulation" || (kind == Backend::MachO && a == "-arch"))
      o.emulator = value();
    else if (a == "--static" || a == "-static" || a == "-Bstatic") {}
    else if (a == "--shared" || a == "-shared" || (kind == Backend::PE && low == "/dll") ||
             (kind == Backend::MachO && a == "-dylib")) o.mode = Mode::Shared;
    else if (a == "--pie" || a == "-pie") o.pie = true;
    else if (a == "-r" || a == "--relocatable") o.mode = Mode::Relocatable;
    else if (a == "--gc-sections") o.gcSections = true;
    else if (a == "--strip-debug" || a == "-s") o.stripDebug = true;
    else if (a == "--allow-undefined") o.allowUndefined = true;
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
  if (o.mode == Mode::Shared || o.pie)
    throw std::runtime_error("native linker does not yet support shared/PIE images or dynamic imports; "
                             "no external linker was invoked");
  if (kind != Backend::ELF && (o.scriptIsText || !o.script.empty()))
    throw std::runtime_error("native PE/Mach-O linker does not support ELF linker scripts");
}
} // namespace qld::detail
