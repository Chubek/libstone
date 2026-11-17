// driver.cpp - top-level link orchestration.
//
// loadInputs -> loadScriptExtras -> discoverSymbols -> mode driver
// (static/shared/relocatable) -> optional -Map write. All file I/O for the
// one-shot API lives in ld.c; this file works on LinkState and byte vectors.
#include "internal.hpp"
#include "ElfBackend.hpp"
#include "ScriptSDK.hpp"

#include <fstream>

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
                 std::string& error) {
  if (needsElfBackend(state.options)) return linkElf(state, out, error, ScriptSDK{});
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
  // A DSO hidden inside a native INPUT/GROUP script must never be copied as
  // relocatable sections. Explicit dynamic mode preserves GNU script semantics.
  for (const auto& object : state.objects) {
    const auto& h = object.bytes;
    if (h.size() >= 18 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' &&
        ((h[5] == 1 && h[16] == 3 && h[17] == 0) || (h[5] == 2 && h[16] == 0 && h[17] == 3))) {
      error = object.label + ": dynamic input in native script; select --dynamic / qld_link_elf";
      return false;
    }
  }
  if (!discoverSymbols(state, error)) return false;
  bool ok = false;
  switch (state.options.mode) {
    case Mode::Exec: ok = linkStatic(state, out, error); break;
    case Mode::Shared: ok = linkShared(state, out, error); break;
    case Mode::Relocatable: ok = linkRelocatable(state, out, error); break;
  }
  if (!ok) return false;
  if (!state.options.mapFile.empty()) {
    std::string m = state.mapText;
    if (m.empty()) m = "(empty map)\n";
    std::vector<uint8_t> mb(m.begin(), m.end());
    std::string werr;
    if (!writeFile(state.options.mapFile, mb, werr)) {
      state.warnings.push_back(werr);
    }
  }
  return true;
}

}  // namespace qld::detail
