// main.cpp - qobjld command-line interface.
//
// A familiar GNU-ld-style driver over the qobjld library: objects and
// archives on the command line, -L/-l search, -T scripts, -e entry,
// -o output, -shared/-pie/-r mode selection and -Map output.
#include "../common/cli.hpp"
#include "../common/options.hpp"

#include <iostream>

#include "ld.h"
#include "ScriptSDK.hpp"
#include "Backend.hpp"

using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};
constexpr OptionSpec kVersion{0, "version", false, "", "print version"};
constexpr OptionSpec kOutput{'o', "output", true, "file", "write output here"};
constexpr OptionSpec kEntry{'e', "entry", true, "symbol", "entry symbol"};
constexpr OptionSpec kScript{'T', "script", true, "file", "linker script file (ld syntax or .lua)"};
constexpr OptionSpec kScriptText{0, "script-text", true, "text",
                                 "linker script text"};
constexpr OptionSpec kLtoScript{0, "lto-script", true, "file",
                               "run a Lua optimization hook for native static ELF"};
constexpr OptionSpec kLibPath{'L', "library-path", true, "dir",
                              "add a -l search directory"};
constexpr OptionSpec kLib{'l', "library", true, "name",
                          "link against lib<name>.a/.so"};
constexpr OptionSpec kMap{0, "Map", true, "file",
                          "write a link map here (-M enables stdout map)"};
constexpr OptionSpec kEmulator{'m', "emulation", true, "name",
                               "accepted emulation name (informational)"};
constexpr OptionSpec kShared{0, "shared", false, "", "emit a shared object"};
constexpr OptionSpec kPie{0, "pie", false, "",
                          "emit a position-independent executable"};
constexpr OptionSpec kReloc{0, "relocatable", false, "",
                            "emit a relocatable object (-r)"};
constexpr OptionSpec kStatic{0, "static", false, "",
                             "emit a static executable (the default)"};
constexpr OptionSpec kGc{0, "gc-sections", false, "",
                         "drop unreferenced non-alloc sections"};
constexpr OptionSpec kStrip{'s', "strip-debug", false, "",
                            "drop debug sections"};
constexpr OptionSpec kAllowUndef{0, "allow-undefined", false, "",
                                 "tolerate undefined symbols"};
constexpr OptionSpec kBase{0, "image-base", true, "addr",
                           "base address (e.g. 0x400000)"};
constexpr OptionSpec kVerbose{'v', "verbose", false, "", "be verbose"};
constexpr OptionSpec kCheckScript{0, "check-script", false, "",
                                   "validate the script and exit"};
constexpr OptionSpec kDynamic{0, "dynamic", false, "", "explicitly use an enabled external compatibility bridge"};
constexpr OptionSpec kElfLinker{0, "elf-linker", true, "executable", "select the ELF toolchain linker"};
constexpr OptionSpec kFormat{0, "format", true, "auto|elf|pe|macho", "select output format (default: input magic)"};
constexpr OptionSpec kPeLinker{0, "pe-linker", true, "executable", "select lld-link-compatible PE backend"};
constexpr OptionSpec kMachOLinker{0, "macho-linker", true, "executable", "select ld64-compatible Mach-O backend"};

// Dynamic invocations retain the original token order, which is essential for
// --as-needed, archive groups, -Bstatic/-Bdynamic and compiler-driver use.
bool elfInvocation(const std::vector<std::string>& args) {
  bool selected = false;
  for (size_t i = 0; i < args.size(); ++i) {
    std::string_view a = args[i];
    if (a == "--help" || a == "--version" || a == "--check-script") return false;
    if (a == "--") break;
    if (a == "--dynamic" || a == "--shared" || a == "-shared" || a == "--pie" || a == "-pie" ||
        a == "--elf-linker" || a.starts_with("--elf-linker=") ||
        a == "--lto-script" || a.starts_with("--lto-script=") ||
        a == "--dynamic-linker" || a == "-dynamic-linker" ||
        a.starts_with("--dynamic-linker=") || a.starts_with("-dynamic-linker=") ||
        a == "--static" || a == "-static" || a == "-Bstatic" || a == "-Bdynamic" ||
        a == "--library" || a.starts_with("--library=") || a.starts_with("-l") ||
        a == "--as-needed" || a == "--no-as-needed" || a == "--start-group" ||
        a == "--whole-archive" || a == "-z" || a.starts_with("--soname") ||
        a.starts_with("--rpath") || a.starts_with("--version-script") ||
        a.starts_with('@')) selected = true;
    i += qld::detail::toolchain::optionArity(args[i]);
  }
  return selected;
}

int elfMain(int argc, char **argv) {
  qld_options_t opts{};
  qld_options_init(&opts);
  opts.output = "a.out";
  qld_elf_options_t elf{};
  qld_elf_options_init(&elf);
  std::vector<const char *> args;
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  elf.arguments = args.data(); elf.n_arguments = args.size();
  elf.diagnostic = [](void *, const char *message) { std::cerr << message; };
  char *error = nullptr;
  int rc = qld_link_elf(&opts, &elf, &error);
  if (rc) std::cerr << "qobjld: " << (error ? error : "ELF link failed") << '\n';
  qld_free_string(error);
  return rc ? 1 : 0;
}

int foreignMain(int argc, char **argv, qld::detail::Backend backend) {
  qld_options_t opts{};
  qld_options_init(&opts);
  opts.output = backend == qld::detail::Backend::PE ? "a.exe" : "a.out";
  qld_elf_options_t config{};
  qld_elf_options_init(&config);
  std::vector<const char *> args;
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  config.arguments = args.data(); config.n_arguments = args.size();
  config.diagnostic = [](void *, const char *message) { std::cerr << message; };
  char *error = nullptr;
  int rc = backend == qld::detail::Backend::PE ? qld_link_pe(&opts, &config, &error)
                                              : qld_link_macho(&opts, &config, &error);
  if (rc) std::cerr << "qobjld: " << (error ? error : "link failed") << '\n';
  qld_free_string(error);
  return rc ? 1 : 0;
}

uint64_t parseAddr(const std::string& t, bool& ok) {
  ok = false;
  if (t.empty()) return 0;
  try {
    size_t used = 0;
    uint64_t v = std::stoull(t, &used, 0);
    if (used != t.size()) return 0;
    ok = true;
    return v;
  } catch (...) {
    return 0;
  }
}

}  // namespace

int main(int argc, char** argv) try {
  std::vector<std::string> raw(argv + 1, argv + argc);
  bool query = false, explicitFormat = false;
  for (size_t i = 0; i < raw.size(); ++i) {
    const auto& a = raw[i];
    if (a == "--") break;
    if (a == "--help" || a == "--version" || a == "--check-script") query = true;
    if (a.starts_with("--format=") || a == "--format") explicitFormat = true;
    i += qld::detail::toolchain::optionArity(a);
  }
  if (!query) {
    auto backend = qld::detail::invocationBackend(raw);
    if (backend == qld::detail::Backend::PE || backend == qld::detail::Backend::MachO)
      return foreignMain(argc, argv, backend);
    if (explicitFormat && backend == qld::detail::Backend::ELF) return elfMain(argc, argv);
  }
  if (elfInvocation(raw)) return elfMain(argc, argv);
  std::vector<OptionSpec> specs = {
      kHelp,      kVersion,   kOutput,    kEntry,     kScript,
      kScriptText, kLtoScript, kLibPath,  kLib,       kMap,       kEmulator,
      kShared,    kPie,       kReloc,     kStatic,    kGc,
      kStrip,     kAllowUndef, kBase,     kVerbose,   kCheckScript, kDynamic, kElfLinker,
      kFormat, kPeLinker, kMachOLinker,
      {'M', "", false, "", "print a link map to stdout"},
      {'r', "", false, "", "emit a relocatable object"},
      {'M', "", false, "", ""},  // placeholder keeps -M visible
  };
  // Deduplicate the -M entry (Options keys on the letter; two entries with
  // 'M' would double-count but remain harmless — keep one).
  specs.pop_back();
  qobj::Options options("qobjld", "inputs... [-o output]", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjld: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  if (options.has(kVersion)) {
    std::cout << "qobjld " << qld_version() << " (qBFD "
              << qobj::kBFDVersion << ")\n";
    return 0;
  }

  // -r short flag shares the relocatable long option.
  bool reloc = options.has(kReloc) || options.has('r');
  bool shared = options.has(kShared);
  bool pie = options.has(kPie);
  if (reloc && (shared || pie)) {
    std::cerr << "qobjld: choose one of -r, --shared and --pie\n";
    return 2;
  }
  if (shared && pie) {
    std::cerr << "qobjld: choose one of --shared and --pie\n";
    return 2;
  }

  if (options.has(kCheckScript)) {
    if (options.has(kLtoScript)) {
      std::cerr << "qobjld: --lto-script needs a link, not --check-script\n";
      return 2;
    }
    std::string text = options.value("script-text");
    if (text.empty() && options.has(kScript)) {
      try {
        auto bytes = qobj::readFile(options.value(kScript));
        text.assign(bytes.begin(), bytes.end());
        text = qld::ScriptSDK{}.preprocess(text, options.value(kScript));
      } catch (const std::exception& e) {
        std::cerr << "qobjld: " << e.what() << '\n';
        return 1;
      }
    }
    if (text.empty()) {
      std::cerr << "qobjld: --check-script needs --script-text or -T\n";
      return 2;
    }
    char* err = nullptr;
    int rc = qld_check_script(text.c_str(), &err);
    if (rc != 0) {
      std::cerr << "qobjld: " << (err ? err : "script error") << '\n';
      qld_free_string(err);
      return 1;
    }
    qld_free_string(err);
    std::cout << "qobjld: script OK\n";
    return 0;
  }

  if (options.positional().empty() && options.valueList(kLib).empty() &&
      !options.has(kScript) && options.value("script-text").empty()) {
    std::cerr << "qobjld: no input files\n\n" << options.usage();
    return 2;
  }

  std::vector<std::string> inputs = options.positional();
  std::vector<std::string> libpaths = options.valueList(kLibPath);
  std::vector<std::string> libs = options.valueList(kLib);

  qld_options_t c{};
  std::vector<const char*> ci, clp, cl;
  for (const auto& s : inputs) ci.push_back(s.c_str());
  for (const auto& s : libpaths) clp.push_back(s.c_str());
  for (const auto& s : libs) cl.push_back(s.c_str());
  c.inputs = ci.empty() ? nullptr : ci.data();
  c.n_inputs = ci.size();
  c.libpaths = clp.empty() ? nullptr : clp.data();
  c.n_libpaths = clp.size();
  c.libs = cl.empty() ? nullptr : cl.data();
  c.n_libs = cl.size();
  std::string output = options.value(kOutput);
  if (output.empty()) output = reloc ? "a.o" : "a.out";
  std::string entry = options.value(kEntry);
  std::string script = options.value(kScript);
  std::string scriptText = options.value("script-text");
  std::string map = options.value("Map");
  std::string emu = options.value(kEmulator);
  c.output = output.c_str();
  c.entry = entry.empty() ? nullptr : entry.c_str();
  c.script = script.empty() ? nullptr : script.c_str();
  c.script_text = scriptText.empty() ? nullptr : scriptText.c_str();
  c.use_script_text = scriptText.empty() ? 0 : 1;
  bool ok = false;
  uint64_t base = 0;
  if (options.has(kBase)) {
    base = parseAddr(options.value(kBase), ok);
    if (!ok) {
      std::cerr << "qobjld: --image-base needs a number\n";
      return 2;
    }
    c.base = base;
    c.has_base = 1;
  }
  c.mode = reloc ? QLD_MODE_RELOCATABLE
           : shared ? QLD_MODE_SHARED : QLD_MODE_EXEC;
  c.pie = pie ? 1 : 0;
  c.gc_sections = options.has(kGc) ? 1 : 0;
  c.strip_debug = options.has(kStrip) ? 1 : 0;
  c.allow_undefined = options.has(kAllowUndef) ? 1 : 0;
  c.verbose = options.has(kVerbose) ? 1 : 0;
  c.map_file = map.empty() ? nullptr : map.c_str();
  c.emulator = emu.empty() ? nullptr : emu.c_str();

  char* err = nullptr;
  int rc = qld_link(&c, &err);
  if (rc != 0) {
    std::cerr << "qobjld: " << (err ? err : "link failed") << '\n';
    qld_free_string(err);
    return 1;
  }
  qld_free_string(err);
  if (options.has('M')) {
    // -M prints the map to stdout: re-read the file when -Map was given,
    // otherwise note that no map file was requested.
    if (!map.empty()) {
      try {
        auto bytes = qobj::readFile(map);
        std::cout.write(reinterpret_cast<const char*>(bytes.data()),
                        std::streamsize(bytes.size()));
      } catch (const std::exception& e) {
        std::cerr << "qobjld: " << e.what() << '\n';
        return 1;
      }
    } else {
      std::cerr << "qobjld: -M needs --Map <file> to capture the map\n";
    }
  }
  if (c.verbose) std::cerr << "qobjld: wrote " << output << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "qobjld: " << e.what() << '\n';
  return 1;
}
