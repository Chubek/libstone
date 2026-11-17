// GNU-compatible ELF bridge. Process/response-file handling is shared with
// PE and Mach-O through Backend.hpp; no shell or platform APIs belong here.
#include "Backend.hpp"
#include "internal.hpp"
#include "ScriptSDK.hpp"

#include <stdexcept>

namespace qld::detail {
const char *defaultElfLinker() {
#ifdef QLD_ENABLE_ELF_BACKEND
  return QLD_DEFAULT_ELF_LINKER;
#else
  return nullptr;
#endif
}

#ifdef QLD_ENABLE_ELF_BACKEND
namespace {
bool extensionScript(const std::string& text, const std::string& path) {
  if (std::filesystem::path(path).extension() == ".lua" || text.starts_with("--!lua")) return true;
  size_t start = 0;
  while (start < text.size()) {
    auto p = text.find_first_not_of(" \t\r", start);
    // INCLUDE must resolve relative to its containing extension script.
    if (p != text.npos && (text[p] == '@' || text.compare(p, 8, "INCLUDE ") == 0 ||
                          text.compare(p, 8, "INCLUDE\t") == 0)) return true;
    auto next = text.find('\n', start);
    if (next == text.npos) break;
    start = next + 1;
  }
  return false;
}
}
#endif

bool linkElf(LinkState& state, std::vector<uint8_t>& out, std::string& error,
             const ScriptSDK& sdk) {
#ifndef QLD_ENABLE_ELF_BACKEND
  (void)state; (void)out; (void)sdk;
  error = "ELF toolchain backend disabled; rebuild with YELLOWSTONE_ENABLE_ELF_BACKEND=ON";
  return false;
#else
  using namespace toolchain;
  try {
    Workspace work;
    auto& o = state.options;
    std::string linker = o.elf.linker.empty() ? defaultElfLinker() : o.elf.linker;
    std::vector<std::string> raw, args;
    if (o.pie) raw.push_back("-pie");
    else if (o.mode == Mode::Shared) raw.push_back("-shared");
    else if (o.mode == Mode::Relocatable) raw.push_back("-r");
    if (o.hasEntry) { raw.push_back("-e"); raw.push_back(o.entry); }
    if (o.hasBase) raw.push_back("-Ttext-segment=" + std::to_string(o.base));
    if (!o.emulator.empty()) { raw.push_back("-m"); raw.push_back(o.emulator); }
    if (o.gcSections) raw.push_back("--gc-sections");
    if (o.stripDebug) raw.push_back("--strip-debug");
    if (o.allowUndefined) raw.push_back("--unresolved-symbols=ignore-all");
    for (const auto& path : o.libPaths) raw.push_back("-L" + path);
    if (o.scriptIsText) raw.push_back("--script-text=" + o.scriptText);
    else if (!o.script.empty()) { raw.push_back("-T"); raw.push_back(o.script); }
    for (const auto& path : o.inputs) raw.push_back(std::filesystem::absolute(path).string());
    for (const auto& lib : o.libs) raw.push_back("-l" + lib);
    raw.insert(raw.end(), o.elf.arguments.begin(), o.elf.arguments.end());
    const auto expanded = expandArguments(raw, Quoting::GNU);
    unsigned scriptNumber = 0;
    auto script = [&](const std::string& text, const std::string& name) {
      auto path = work.path / ("script" + std::to_string(scriptNumber++) + ".ld");
      writeText(path, sdk.preprocess(text, name));
      return path.string();
    };
    auto expected = o.pie || o.mode == Mode::Shared ? qbfd::FileType::SharedObject :
                    o.mode == Mode::Relocatable ? qbfd::FileType::Relocatable : qbfd::FileType::Executable;
    bool staticOnly = false;
    for (size_t i = 0; i < expanded.size(); ++i) {
      const auto& arg = expanded[i];
      auto value = [&]() -> const std::string& {
        if (++i == expanded.size()) throw std::runtime_error("missing argument for " + arg);
        return expanded[i];
      };
      if (arg == "--dynamic") continue;
      if (arg == "--format" || arg.starts_with("--format=")) {
        auto requested = namedBackend(arg == "--format" ? value() : arg.substr(9));
        if (requested != Backend::Auto && requested != Backend::ELF)
          throw std::runtime_error("conflicting linker formats");
        continue;
      }
      if (arg == "--elf-linker") { linker = value(); continue; }
      if (arg.starts_with("--elf-linker=")) { linker = arg.substr(13); continue; }
      if (arg.starts_with("--pe-linker") || arg.starts_with("--macho-linker"))
        throw std::runtime_error("conflicting linker backends");
      if (arg == "-o" || arg == "--output" || arg == "-output") { o.output = value(); continue; }
      if (arg.starts_with("--output=") || arg.starts_with("-output=")) { o.output = arg.substr(arg.find('=') + 1); continue; }
      if (arg.starts_with("-o") && !arg.starts_with("--") && !arg.starts_with("-output") &&
          !arg.starts_with("-orphan") && !arg.starts_with("-oformat")) {
        o.output = arg.substr(2); continue;
      }
      if (arg == "-Map" || arg == "--Map") { o.mapFile = value(); continue; }
      if (arg.starts_with("-Map=") || arg.starts_with("--Map=")) { o.mapFile = arg.substr(arg.find('=') + 1); continue; }
      if (arg.starts_with("-Map") && arg.size() > 4) { o.mapFile = arg.substr(4); continue; }
      if (arg == "--allow-undefined") { args.push_back("--unresolved-symbols=ignore-all"); continue; }
      if (arg == "--image-base" || arg.starts_with("--image-base=")) {
        args.push_back("-Ttext-segment=" + (arg == "--image-base" ? value() : arg.substr(13))); continue;
      }
      if (arg == "--script-text" || arg.starts_with("--script-text=")) {
        auto text = arg == "--script-text" ? value() : arg.substr(14);
        args.push_back("-T"); args.push_back(script(text, "<script>")); continue;
      }
      if (arg == "-T" || arg == "--script" || arg.starts_with("--script=") ||
          (arg.starts_with("-T") && !arg.starts_with("-Ttext") && !arg.starts_with("-Tdata") && !arg.starts_with("-Tbss"))) {
        auto path = (arg == "-T" || arg == "--script") ? value() :
                    arg.starts_with("--script=") ? arg.substr(9) : arg.substr(2);
        auto text = readText(path);
        args.push_back("-T");
        args.push_back(extensionScript(text, path) ? script(text, path) : path);
        continue;
      }
      if (arg == "--") {
        while (++i < expanded.size()) args.push_back(std::filesystem::absolute(expanded[i]).string());
        break;
      }
      if (arg == "--shared" || arg == "-shared" || arg == "--pie" || arg == "-pie")
        expected = qbfd::FileType::SharedObject;
      if (arg == "-r" || arg == "--relocatable" || arg == "-i")
        expected = qbfd::FileType::Relocatable;
      if (arg == "--no-pie" || arg == "-no-pie") expected = qbfd::FileType::Executable;
      if (arg == "--static" || arg == "-static" || arg == "-dn" || arg == "-non_shared") staticOnly = true;
      if (arg == "-Bdynamic" || arg == "-dy" || arg == "-call_shared") staticOnly = false;
      args.push_back(arg);
      for (unsigned n = optionArity(arg); n; --n) args.push_back(value());
    }
    checkDestinations(o.output, {o.mapFile});
    const auto image = work.path / "image.elf";
    args.push_back("-o"); args.push_back(image.string());
    if (!o.mapFile.empty()) { args.push_back("-Map"); args.push_back((work.path / "map").string()); }
    auto diagnostics = runProcess(linker, args, work, Quoting::GNU, "ELF");
    rejectIgnoredOptions(diagnostics);
    auto result = qbfd::open(image.string());
    if (!result || result->object->format() != qbfd::Format::ELF)
      throw std::runtime_error("ELF linker produced no valid ELF image\n" + diagnostics);
    if (result->object->fileType() != expected)
      throw std::runtime_error("ELF linker produced the wrong output kind");
    if (staticOnly) {
      if (!result->object->neededLibraries().empty())
        throw std::runtime_error("static ELF output has dynamic dependencies");
      for (const auto& segment : result->object->segments())
        if (segment.nativeType == 3) throw std::runtime_error("static ELF output has a dynamic interpreter");
    }
    std::string map;
    if (!o.mapFile.empty()) map = readText(work.path / "map");
    if (o.elf.diagnostic && !diagnostics.empty()) o.elf.diagnostic(diagnostics);
    auto bytes = result->buffer->span();
    std::vector<uint8_t> validated(bytes.begin(), bytes.end());
    publish(work.path / "map", o.mapFile);
    state.mapText = std::move(map);
    out = std::move(validated);
    return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
#endif
}
} // namespace qld::detail
