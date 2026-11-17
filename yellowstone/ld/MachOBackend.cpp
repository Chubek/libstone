// Darwin ld64-compatible backend. Preserve native argv order for frameworks,
// weak/reexport libraries, two-level namespaces and archive loading policies.
#include "Backend.hpp"
#include "internal.hpp"
#include <sstream>
#include <stdexcept>

namespace qld::detail {
const char *defaultMachOLinker() {
#ifdef QLD_ENABLE_MACHO_BACKEND
  return QLD_DEFAULT_MACHO_LINKER;
#else
  return nullptr;
#endif
}

bool linkMachO(LinkState& state, std::vector<uint8_t>& out, std::string& error) {
#ifndef QLD_ENABLE_MACHO_BACKEND
  (void)state; (void)out;
  error = "Mach-O toolchain backend disabled; rebuild with YELLOWSTONE_ENABLE_MACHO_BACKEND=ON";
  return false;
#else
  using namespace toolchain;
  try {
    auto& o = state.options;
    if (o.scriptIsText || !o.script.empty())
      throw std::runtime_error("Mach-O backend does not support GNU/Lua linker scripts; use native segment/section options");
    Workspace work;
    std::string linker = o.macho.linker.empty() ? defaultMachOLinker() : o.macho.linker;
    uint32_t kind = o.mode == Mode::Relocatable ? 1 : o.mode == Mode::Shared && !o.pie ? 6 : 2;
    bool staticOnly = false, hasInstallName = false;
    std::vector<std::string> raw, args;
    if (kind == 1) raw.push_back("-r");
    if (kind == 6) raw.push_back("-dylib");
    if (o.pie) raw.push_back("-pie");
    if (o.hasEntry) { raw.push_back("-e"); raw.push_back(o.entry); }
    if (o.hasBase) {
      // ld64 address arguments are hexadecimal, without a required 0x prefix.
      std::ostringstream address; address << std::hex << o.base;
      raw.push_back("-seg1addr"); raw.push_back(address.str());
    }
    if (!o.emulator.empty()) { raw.push_back("-arch"); raw.push_back(o.emulator); }
    if (o.gcSections) raw.push_back("-dead_strip");
    if (o.stripDebug) raw.push_back("-S");
    if (o.allowUndefined) { raw.push_back("-undefined"); raw.push_back("dynamic_lookup"); }
    for (const auto& p : o.libPaths) raw.push_back("-L" + p);
    for (const auto& p : o.inputs) raw.push_back(std::filesystem::absolute(p).string());
    for (const auto& lib : o.libs) raw.push_back("-l" + lib);
    raw.insert(raw.end(), o.macho.arguments.begin(), o.macho.arguments.end());
    auto expanded = expandArguments(raw);
    for (size_t i = 0; i < expanded.size(); ++i) {
      const auto& a = expanded[i];
      auto value = [&]() -> const std::string& {
        if (++i == expanded.size()) throw std::runtime_error("missing argument for " + a);
        return expanded[i];
      };
      if (a == "--format" || a.starts_with("--format=")) {
        auto requested = namedBackend(a == "--format" ? value() : a.substr(9));
        if (requested != Backend::Auto && requested != Backend::MachO)
          throw std::runtime_error("conflicting linker formats");
        continue;
      }
      if (a == "--dynamic") continue;
      if (a == "--macho-linker") { linker = value(); continue; }
      if (a.starts_with("--macho-linker=")) { linker = a.substr(15); continue; }
      if (a.starts_with("--elf-linker") || a.starts_with("--pe-linker"))
        throw std::runtime_error("conflicting linker backends");
      if (a == "-o" || a == "--output") { o.output = value(); continue; }
      if (a.starts_with("--output=")) { o.output = a.substr(9); continue; }
      if (a == "-Map" || a == "--Map" || a == "-map") { o.mapFile = value(); continue; }
      if (a.starts_with("-Map=") || a.starts_with("--Map=")) { o.mapFile = a.substr(a.find('=') + 1); continue; }
      if (a == "-T" || a.starts_with("--script") || a.starts_with("-T"))
        throw std::runtime_error("Mach-O backend does not support GNU/Lua linker scripts");
      if (a == "--shared" || a == "-shared") { args.push_back("-dylib"); kind = 6; continue; }
      if (a == "-dylib") kind = 6;
      if (a == "-bundle") kind = 8;
      if (a == "-execute") kind = 2;
      if (a == "--relocatable") { args.push_back("-r"); kind = 1; continue; }
      if (a == "-r") kind = 1;
      if (a == "--static") { args.push_back("-static"); staticOnly = true; continue; }
      if (a == "-static") staticOnly = true;
      if (a == "--pie") { args.push_back("-pie"); continue; }
      if (a == "--gc-sections") { args.push_back("-dead_strip"); continue; }
      if (a == "--strip-debug" || a == "-s") { args.push_back("-S"); continue; }
      if (a == "--allow-undefined") { args.push_back("-undefined"); args.push_back("dynamic_lookup"); continue; }
      if (a == "--verbose") { args.push_back("-v"); continue; }
      if (a == "--entry") { args.push_back("-e"); args.push_back(value()); continue; }
      if (a.starts_with("--entry=")) { args.push_back("-e"); args.push_back(a.substr(8)); continue; }
      if (a == "--image-base" || a.starts_with("--image-base=")) {
        const auto text = a == "--image-base" ? value() : a.substr(13);
        size_t used = 0;
        auto number = std::stoull(text, &used, 0);
        if (used != text.size()) throw std::runtime_error("invalid image base: " + text);
        std::ostringstream address; address << std::hex << number;
        args.push_back("-seg1addr"); args.push_back(address.str()); continue;
      }
      if (a == "-m" || a == "--emulation") { args.push_back("-arch"); args.push_back(value()); continue; }
      if (a == "--library-path") { args.push_back("-L" + value()); continue; }
      if (a.starts_with("--library-path=")) { args.push_back("-L" + a.substr(15)); continue; }
      if (a == "--library") { args.push_back("-l" + value()); continue; }
      if (a.starts_with("--library=")) { args.push_back("-l" + a.substr(10)); continue; }
      if (a == "-install_name" || a == "-dylib_install_name" ||
          a == "-dylinker_install_name" || a == "-final_output") hasInstallName = true;
      if (a == "--") {
        while (++i < expanded.size()) args.push_back(std::filesystem::absolute(expanded[i]).string());
        break;
      }
      args.push_back(a);
      for (unsigned n = optionArity(a); n; --n) args.push_back(value());
    }
    // ld64 otherwise warns and discards wrong-architecture inputs.
    args.push_back("-arch_errors_fatal");
    const auto image = work.path / "image.macho";
    args.push_back("-o"); args.push_back(image.string());
    if (kind == 6 && !hasInstallName) {
      args.push_back("-install_name");
      args.push_back(o.output.empty() ? "qobjld.dylib" : o.output);
    }
    checkDestinations(o.output, {o.mapFile});
    if (!o.mapFile.empty()) { args.push_back("-map"); args.push_back((work.path / "map").string()); }
    auto diagnostics = runProcess(linker, args, work, Quoting::GNU, "Mach-O");
    auto result = qbfd::open(image);
    if (!result || result->object->format() != qbfd::Format::MachO)
      throw std::runtime_error("Mach-O linker produced no valid Mach-O image\n" + diagnostics);
    auto bytes = result->buffer->span();
    const auto& macho = static_cast<const qbfd::macho::MachOFile&>(*result->object);
    qbfd::detail::Reader reader{bytes};
    reader.order = macho.endian();
    if (reader.u32(12) != kind)
      throw std::runtime_error("Mach-O linker did not produce the requested output kind (partial linking may be unsupported)\n" + diagnostics);
    if (staticOnly && ((macho.flags() & 4) || !macho.neededLibraries().empty()))
      throw std::runtime_error("Mach-O linker does not support fully static output (-static); static archives can be linked into dynamic images\n" + diagnostics);
    rejectIgnoredOptions(diagnostics);
    out.assign(bytes.begin(), bytes.end());
    if (!o.mapFile.empty()) {
      state.mapText = readText(work.path / "map");
      publish(work.path / "map", o.mapFile);
    }
    if (o.macho.diagnostic && !diagnostics.empty()) o.macho.diagnostic(diagnostics);
    return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
#endif
}
} // namespace qld::detail
