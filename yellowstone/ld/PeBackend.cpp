// PE/COFF linking with an lld-link-compatible toolchain. COFF COMDAT selection,
// relocations, import thunks, TLS, unwind tables and exports belong to that
// linker; qld owns option translation, staging, validation and publication.
#include "Backend.hpp"
#include "internal.hpp"
#include <stdexcept>

namespace qld::detail {
const char *defaultPeLinker() {
#ifdef QLD_ENABLE_PE_BACKEND
  return QLD_DEFAULT_PE_LINKER;
#else
  return nullptr;
#endif
}

bool linkPe(LinkState& state, std::vector<uint8_t>& out, std::string& error) {
#ifndef QLD_ENABLE_PE_BACKEND
  (void)state; (void)out;
  error = "PE toolchain backend disabled; rebuild with YELLOWSTONE_ENABLE_PE_BACKEND=ON";
  return false;
#else
  using namespace toolchain;
  try {
    auto& o = state.options;
    if (o.scriptIsText || !o.script.empty())
      throw std::runtime_error("PE backend does not support GNU/Lua linker scripts; use /def and native section options");
    if (o.mode == Mode::Relocatable)
      throw std::runtime_error("PE/COFF partial linking (-r) is unsupported by lld-link; use an archive for static libraries");
    Workspace work;
    std::string linker = o.pe.linker.empty() ? defaultPeLinker() : o.pe.linker;
    bool dll = o.mode == Mode::Shared && !o.pie;
    bool staticOnly = false, noImplib = false, debug = false, pdbAlt = false;
    bool manifest = false, map = !o.mapFile.empty();
    std::string implib, pdb, manifestFile;
    std::vector<std::string> raw, args;
    if (dll) raw.push_back("/dll");
    if (o.pie) raw.push_back("/dynamicbase");
    if (o.hasEntry) raw.push_back("/entry:" + o.entry);
    if (o.hasBase) raw.push_back("/base:" + std::to_string(o.base));
    if (o.gcSections) raw.push_back("/opt:ref");
    if (o.stripDebug) raw.push_back("/debug:none");
    if (o.allowUndefined) raw.push_back("/force:unresolved");
    if (!o.emulator.empty()) {
      if (o.emulator == "i386pep") raw.push_back("/machine:x64");
      else if (o.emulator == "i386pe") raw.push_back("/machine:x86");
      else raw.push_back("/machine:" + o.emulator);
    }
    for (const auto& p : o.libPaths) raw.push_back("-L" + p);
    for (const auto& p : o.inputs) raw.push_back(std::filesystem::absolute(p).string());
    for (const auto& lib : o.libs) raw.push_back("-l" + lib);
    raw.insert(raw.end(), o.pe.arguments.begin(), o.pe.arguments.end());
    auto expanded = expandArguments(raw, Quoting::Windows);
    std::vector<std::string> libPaths;
    // -L is global, even when it follows -l. Preserve library operand order.
    for (size_t i = 0; i < expanded.size(); ++i) {
      const auto& a = expanded[i];
      if (a == "--") break;
      if ((a == "-L" || a == "--library-path") && i + 1 < expanded.size()) libPaths.push_back(expanded[++i]);
      else if (a.starts_with("-L")) libPaths.push_back(a.substr(2));
      else if (a.starts_with("--library-path=")) libPaths.push_back(a.substr(15));
      else if (lower(a).starts_with("/libpath:")) libPaths.push_back(a.substr(9));
    }
    auto library = [&](const std::string& name) {
      if (name.empty()) throw std::runtime_error("empty library name");
      std::vector<std::string> names;
      if (name[0] == ':') names.push_back(name.substr(1));
      else names = {name + ".lib", "lib" + name + ".lib", "lib" + name + ".a", "lib" + name + ".dll.a"};
      for (const auto& dir : libPaths)
        for (const auto& candidate : names) {
          auto path = std::filesystem::path(dir) / candidate;
          if (std::filesystem::is_regular_file(path)) return std::filesystem::absolute(path).string();
        }
      // Native COFF default library lookup includes /libpath and LIB.
      return names.front();
    };
    for (size_t i = 0; i < expanded.size(); ++i) {
      const auto& a = expanded[i];
      auto low = lower(a);
      auto value = [&]() -> const std::string& {
        if (++i == expanded.size()) throw std::runtime_error("missing argument for " + a);
        return expanded[i];
      };
      if (a == "--format" || a.starts_with("--format=")) {
        auto requested = namedBackend(a == "--format" ? value() : a.substr(9));
        if (requested != Backend::Auto && requested != Backend::PE)
          throw std::runtime_error("conflicting linker formats");
        continue;
      }
      if (a == "--dynamic") continue;
      if (a == "--pe-linker") { linker = value(); continue; }
      if (a.starts_with("--pe-linker=")) { linker = a.substr(12); continue; }
      if (a.starts_with("--elf-linker") || a.starts_with("--macho-linker"))
        throw std::runtime_error("conflicting linker backends");
      if (a == "-o" || a == "--output") { o.output = value(); continue; }
      if (a.starts_with("--output=")) { o.output = a.substr(9); continue; }
      if (low.starts_with("/out:") || low.starts_with("-out:")) { o.output = a.substr(5); continue; }
      if (a.starts_with("-o") && !a.starts_with("--") && !a.starts_with("-opt:")) {
        o.output = a.substr(2); continue;
      }
      if (a == "-Map" || a == "--Map" || a == "-map") { o.mapFile = value(); map = true; continue; }
      if (a.starts_with("-Map=") || a.starts_with("--Map=")) { o.mapFile = a.substr(a.find('=') + 1); map = true; continue; }
      if (low == "/map" || low == "-map") { map = true; continue; }
      if (low.starts_with("/map:") || low.starts_with("-map:")) { o.mapFile = a.substr(5); map = true; continue; }
      if (a == "--out-implib") { implib = value(); continue; }
      if (a.starts_with("--out-implib=")) { implib = a.substr(13); continue; }
      if (low.starts_with("/implib:") || low.starts_with("-implib:")) { implib = a.substr(8); continue; }
      if (low == "/noimplib" || low == "-noimplib") noImplib = true;
      if (low.starts_with("/pdb:") || low.starts_with("-pdb:")) { pdb = a.substr(5); continue; }
      if (low.starts_with("/pdbaltpath:") || low.starts_with("-pdbaltpath:")) pdbAlt = true;
      if (low == "/debug" || low.starts_with("/debug:") || low == "-debug" || low.starts_with("-debug:"))
        debug = low != "/debug:none" && low != "-debug:none";
      if (low.starts_with("/manifestfile:") || low.starts_with("-manifestfile:")) { manifestFile = a.substr(14); continue; }
      if (low == "/manifest" || low == "-manifest") manifest = true;
      if (low.starts_with("/manifest:") || low.starts_with("-manifest:")) manifest = false;
      if (a == "--shared" || a == "-shared") { args.push_back("/dll"); dll = true; continue; }
      if (low == "/dll" || low == "-dll") dll = true;
      if (a == "--pie" || a == "-pie") { args.push_back("/dynamicbase"); continue; }
      if (a == "--static" || a == "-static") { staticOnly = true; continue; }
      if (a == "-r" || a == "--relocatable")
        throw std::runtime_error("PE/COFF partial linking (-r) is unsupported by lld-link; use an archive for static libraries");
      if (a == "-T" || a.starts_with("--script") || a.starts_with("-T"))
        throw std::runtime_error("PE backend does not support GNU/Lua linker scripts; use /def");
      if (a == "-e" || a == "--entry") { args.push_back("/entry:" + value()); continue; }
      if (a.starts_with("--entry=")) { args.push_back("/entry:" + a.substr(8)); continue; }
      if (a == "--image-base") { args.push_back("/base:" + value()); continue; }
      if (a.starts_with("--image-base=")) { args.push_back("/base:" + a.substr(13)); continue; }
      if (a == "--subsystem") { args.push_back("/subsystem:" + value()); continue; }
      if (a.starts_with("--subsystem=")) { args.push_back("/subsystem:" + a.substr(12)); continue; }
      if (a == "--export") { args.push_back("/export:" + value()); continue; }
      if (a.starts_with("--export=")) { args.push_back("/export:" + a.substr(9)); continue; }
      if (a == "--gc-sections") { args.push_back("/opt:ref"); continue; }
      if (a == "-s" || a == "--strip-debug") { args.push_back("/debug:none"); debug = false; continue; }
      if (a == "--allow-undefined") { args.push_back("/force:unresolved"); continue; }
      if (a == "-v" || a == "--verbose") { args.push_back("/verbose"); continue; }
      if (a == "-m" || a == "--emulation") {
        auto m = value();
        args.push_back("/machine:" + (m == "i386pep" ? "x64" : m == "i386pe" ? "x86" : m)); continue;
      }
      if (a == "-L" || a == "--library-path") { args.push_back("/libpath:" + value()); continue; }
      if (a.starts_with("-L")) { args.push_back("/libpath:" + a.substr(2)); continue; }
      if (a.starts_with("--library-path=")) { args.push_back("/libpath:" + a.substr(15)); continue; }
      if (a == "-l" || a == "--library") { args.push_back(library(value())); continue; }
      if (a.starts_with("-l") && !a.starts_with("-lib")) { args.push_back(library(a.substr(2))); continue; }
      if (a.starts_with("--library=")) { args.push_back(library(a.substr(10))); continue; }
      if (low == "/lib" || low == "-lib")
        throw std::runtime_error("PE backend links images; use qobjar/llvm-lib to create static libraries");
      if (a == "--") {
        while (++i < expanded.size()) args.push_back(std::filesystem::absolute(expanded[i]).string());
        break;
      }
      args.push_back(a);
    }
    auto logical = std::filesystem::path(o.output.empty() ? (dll ? "qobjld.dll" : "qobjld.exe") : o.output);
    auto imageDir = work.path / "image";
    std::filesystem::create_directory(imageDir);
    auto image = imageDir / logical.filename();
    if (logical.filename().empty()) throw std::runtime_error("empty PE output filename");
    if (!o.output.empty()) {
      if (implib.empty()) { auto p = logical; implib = p.replace_extension(".lib").string(); }
      if (pdb.empty()) { auto p = logical; pdb = p.replace_extension(".pdb").string(); }
      if (map && o.mapFile.empty()) { auto p = logical; o.mapFile = p.replace_extension(".map").string(); }
      if (manifestFile.empty()) manifestFile = logical.string() + ".manifest";
    }
    checkDestinations(o.output, {map ? o.mapFile : "", noImplib ? "" : implib,
                                debug ? pdb : "", manifest ? manifestFile : ""});
    // Staging must retain the final basename: it is embedded in the export
    // directory and in the import library's DLL name.
    args.push_back("/out:" + image.string());
    if (!noImplib) args.push_back("/implib:" + (work.path / "import.lib").string());
    if (map) args.push_back("/map:" + (work.path / "map").string());
    if (debug) {
      args.push_back("/pdb:" + (work.path / "debug.pdb").string());
      if (!pdbAlt) args.push_back("/pdbaltpath:" + (pdb.empty() ? logical.stem().string() + ".pdb" : pdb));
    }
    if (manifest) args.push_back("/manifestfile:" + (work.path / "manifest").string());
    auto diagnostics = runProcess(linker, args, work, Quoting::Windows, "PE");
    auto result = qbfd::open(image);
    if (!result || result->object->format() != qbfd::Format::PE)
      throw std::runtime_error("PE linker produced no valid PE image\n" + diagnostics);
    const auto& pe = static_cast<const qbfd::pe::PEFile&>(*result->object);
    if (pe.fileType() != (dll ? qbfd::FileType::SharedObject : qbfd::FileType::Executable))
      throw std::runtime_error("PE linker produced the wrong output kind\n" + diagnostics);
    if (staticOnly) {
      const auto& dirs = pe.dataDirectories();
      // Import and delay-import directories, including ordinal-only imports.
      if ((dirs.size() > 1 && dirs[1].size) || (dirs.size() > 13 && dirs[13].size))
        throw std::runtime_error("PE --static output contains DLL imports; use static libraries");
    }
    rejectIgnoredOptions(diagnostics);
    auto bytes = result->buffer->span();
    out.assign(bytes.begin(), bytes.end());
    if (map) {
      state.mapText = readText(work.path / "map");
      publish(work.path / "map", o.mapFile);
    }
    if (!noImplib && std::filesystem::exists(work.path / "import.lib")) publish(work.path / "import.lib", implib);
    if (debug) publish(work.path / "debug.pdb", pdb);
    if (manifest) publish(work.path / "manifest", manifestFile);
    if (o.pe.diagnostic && !diagnostics.empty()) o.pe.diagnostic(diagnostics);
    return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
#endif
}
} // namespace qld::detail
