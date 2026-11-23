// driver.cpp - top-level link orchestration.
//
// loadInputs -> loadScriptExtras -> discoverSymbols -> mode driver
// (static/shared/relocatable) -> optional -Map write. All file I/O for the
// one-shot API lives in ld.c; this file works on LinkState and byte vectors.
#include "internal.hpp"
#include "ElfBackend.hpp"
#include "Backend.hpp"
#include "ScriptSDK.hpp"

#include <fstream>
#include <exception>

namespace qld::detail {

static bool writeFile(const std::string& path, const std::vector<uint8_t>& b,
                      std::string& error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(b.data()),
            std::streamsize(b.size()));
  out.close();
  if (!out) {
    error = "write failed: " + path;
    return false;
  }
  return true;
}

bool linkState(LinkState& state, std::vector<uint8_t>& out,
                  std::string& error, const ScriptSDK *sdk) {
  try {
    auto backend = selectBackend(state.options);
    if (backend == Backend::Auto) backend = Backend::ELF;
    if (configureLinkerArguments(state.options, backend)) {
      switch (backend) {
        case Backend::ELF: return linkElf(state, out, error, sdk ? *sdk : ScriptSDK{});
        case Backend::PE: return linkPe(state, out, error);
        case Backend::MachO: return linkMachO(state, out, error);
        case Backend::Auto: break;
      }
    }
    state.format = backend == Backend::PE ? qbfd::Format::COFF :
                   backend == Backend::MachO ? qbfd::Format::MachO : qbfd::Format::ELF;
    toolchain::checkDestinations(state.options.output, {state.options.mapFile, state.options.ltoScript});
    if (sdk && (state.options.scriptIsText || !state.options.script.empty())) {
      state.options.scriptText = sdk->preprocess(state.options.scriptIsText ?
          state.options.scriptText : toolchain::readText(state.options.script),
          state.options.scriptIsText ? "<script>" : state.options.script);
      state.options.scriptIsText = true;
    }
  } catch (const std::exception& e) { error = e.what(); return false; }
  // Explicit scripts can supply INPUT and SEARCH_DIR before -l resolution.
  if (state.options.scriptIsText || !state.options.script.empty())
    if (!loadScriptExtras(state, error)) return false;
  if (!loadInputs(state, error)) return false;
  if (hasScript(state) && !state.scriptPrepared) {
    if (!loadScriptExtras(state, error)) return false;
    // Geometry may have changed with new objects; revalidate class/order.
    for (const auto& o : state.objects) {
      if (o.isScript || !o.object) continue;
      if (o.object->format() != qbfd::Format::ELF || o.object->is64Bit() != state.wide ||
          o.object->endian() != state.endian || o.object->arch() != state.arch) {
        error = o.label + ": format/class/order/architecture mismatch with script inputs";
        return false;
      }
    }
  }
  if (!discoverSymbols(state, error)) return false;
  if (state.format == qbfd::Format::ELF) {
    if (!state.options.emulator.empty()) {
      const auto& emulation = state.options.emulator;
      bool match = (state.arch == qbfd::Arch::X86_64 && emulation == "elf_x86_64") ||
                   (state.arch == qbfd::Arch::X86 && emulation == "elf_i386") ||
                   (state.arch == qbfd::Arch::RISCV64 && emulation == "elf64lriscv") ||
                   (state.arch == qbfd::Arch::AArch64 && emulation == "aarch64linux");
      if (!match) {
        error = "unsupported native ELF emulation or architecture mismatch: " + emulation;
        return false;
      }
    }
    for (const auto& input : state.objects) {
      if (!input.object) continue;
      if (input.object->fileType() == qbfd::FileType::SharedObject) {
        for (const auto& section : input.object->sections())
          if (section.name == ".gnu.version" ||
              section.name == ".gnu.version_d" ||
              section.name == ".gnu.version_r") {
            error = input.label +
                ": native ELF symbol-version dependencies are not yet supported";
            return false;
          }
        continue;
      }
      for (const auto& section : input.object->sections()) {
        if ((section.flags & qbfd::sec::Tls) || section.nativeType == 17) {
          error = input.label + ": native ELF TLS/COMDAT processing is not yet supported";
          return false;
        }
      }
      for (const auto& symbol : input.object->symbols()) {
        if (symbol.name.find('@') != std::string::npos) {
          error = input.label +
              ": native ELF symbol-version aliases are not yet supported";
          return false;
        }
        if (symbol.kind == qbfd::SymbolKind::Indirect) {
          error = input.label + ": native ELF IFUNC processing is not yet supported";
          return false;
        }
      }
    }
  }
  if (!state.options.ltoScript.empty()) {
    if (state.options.entry.empty()) state.options.entry = "_start";
    if (!runLtoScript(state, error)) return false;
  }
  bool ok = false;
  if (state.format != qbfd::Format::ELF) {
    ok = linkNativeImage(state, out, error);
  } else switch (state.options.mode) {
    case Mode::Exec:
      ok = state.options.pie || !state.dynamicObjects.empty()
               ? linkShared(state, out, error)
               : linkStatic(state, out, error);
      break;
    case Mode::Shared: ok = linkShared(state, out, error); break;
    case Mode::Relocatable: ok = linkRelocatable(state, out, error); break;
  }
  if (!ok) return false;
  if (!state.options.mapFile.empty() && state.options.ltoScript.empty()) {
    std::string m = state.mapText;
    if (m.empty()) m = "(empty map)\n";
    std::vector<uint8_t> mb(m.begin(), m.end());
    std::string werr;
    if (!writeFile(state.options.mapFile, mb, werr)) {
      error = werr;
      return false;
    }
  }
  return true;
}

}  // namespace qld::detail
