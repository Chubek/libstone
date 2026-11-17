// Complete dynamic ELF linking through a GNU-compatible toolchain linker.
// The child writes only a private staged image; qld's ordinary output path is
// published after successful exit and ELF validation. Never invoke a shell.
#include "ElfBackend.hpp"
#include "internal.hpp"
#include "ScriptSDK.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

#ifdef QLD_ENABLE_ELF_BACKEND
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace qld::detail {
const char *defaultElfLinker() {
#ifdef QLD_ENABLE_ELF_BACKEND
  return QLD_DEFAULT_ELF_LINKER;
#else
  return nullptr;
#endif
}

bool needsElfBackend(const LinkOptions& o) {
  if (o.elf.enabled || o.mode == Mode::Shared || o.pie ||
      (!o.libs.empty() && defaultElfLinker())) return true;
  for (const auto& path : o.inputs) {
    // Inspect e_type, including stripped DSOs and filenames without .so.
    std::ifstream in(path, std::ios::binary);
    std::array<unsigned char, 18> h{};
    if (in.read(reinterpret_cast<char *>(h.data()), h.size()) &&
        h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' &&
        ((h[5] == 1 && h[16] == 3 && h[17] == 0) ||
         (h[5] == 2 && h[16] == 0 && h[17] == 3))) return true;
  }
  return false;
}

#ifdef QLD_ENABLE_ELF_BACKEND
namespace {
constexpr size_t sourceLimit = 16 * 1024 * 1024;
constexpr size_t diagnosticLimit = 1024 * 1024;

std::string readText(const std::filesystem::path& path, size_t limit) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 0 || uint64_t(in.tellg()) > limit)
    throw std::runtime_error("cannot read file (or size limit exceeded): " + path.string());
  std::string text(static_cast<size_t>(in.tellg()), '\0');
  in.seekg(0);
  if (!text.empty() && !in.read(text.data(), std::streamsize(text.size())))
    throw std::runtime_error("read failed: " + path.string());
  if (text.find('\0') != text.npos)
    throw std::runtime_error("NUL byte in source: " + path.string());
  return text;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
  out.close();
  if (!out) throw std::runtime_error("cannot write " + path.string());
}

struct Workspace {
  std::filesystem::path path;
  Workspace() {
    auto pattern = (std::filesystem::temp_directory_path() / "qobjld-XXXXXX").string();
    if (!::mkdtemp(pattern.data()))
      throw std::runtime_error("cannot create linker workspace: " + std::string(std::strerror(errno)));
    path = pattern;
  }
  ~Workspace() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

// GNU response-file syntax: whitespace, single/double quotes, backslash
// escapes. Expansion is bounded and cycles are errors, never subprocesses.
std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> result;
  std::string word;
  char quote = 0;
  bool started = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\\') {
      if (++i == text.size()) throw std::runtime_error("trailing escape in response file");
      word += text[i]; started = true;
    } else if (quote) {
      if (c == quote) quote = 0; else word += c;
    } else if (c == '\'' || c == '"') {
      quote = c; started = true;
    } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
      if (started) { result.push_back(std::move(word)); word.clear(); started = false; }
    } else { word += c; started = true; }
  }
  if (quote) throw std::runtime_error("unterminated quote in response file");
  if (started) result.push_back(std::move(word));
  return result;
}

void expandArguments(const std::vector<std::string>& args, std::vector<std::string>& out,
                     std::set<std::filesystem::path>& active, size_t& budget, unsigned depth = 0) {
  if (depth > 64) throw std::runtime_error("response file nesting limit exceeded");
  for (const auto& arg : args) {
    if (arg.size() + 1 > budget) throw std::runtime_error("linker argument size limit exceeded");
    budget -= arg.size() + 1;
    if (arg.starts_with('@') && (out.empty() || out.back() != "--script-text")) {
      auto path = std::filesystem::canonical(arg.substr(1));
      if (!active.insert(path).second) throw std::runtime_error("response file cycle: " + path.string());
      auto text = readText(path, budget);
      expandArguments(words(text), out, active, budget, depth + 1);
      active.erase(path);
    } else { out.push_back(arg); }
  }
}

std::string runProcess(const std::string& linker, std::vector<std::string> args,
                       const Workspace& work) {
  if (std::getenv("QLD_ELF_BACKEND_ACTIVE"))
    throw std::runtime_error("recursive qobjld ELF backend invocation; select a toolchain linker");
  // A private response file avoids ARG_MAX/per-argument limits for large
  // archive groups. Quote every byte-sensitive token using GNU's grammar.
  std::string response;
  for (const auto& arg : args) {
    response += '"';
    for (char c : arg) {
      if (c == '\\' || c == '"') response += '\\';
      response += c;
    }
    response += "\"\n";
  }
  writeText(work.path / "arguments.rsp", response);
  args = {linker, "@" + (work.path / "arguments.rsp").string()};
  std::vector<char *> argv;
  for (auto& arg : args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  std::vector<char *> env;
  for (char **p = environ; *p; ++p) env.push_back(*p);
  char marker[] = "QLD_ELF_BACKEND_ACTIVE=1";
  env.push_back(marker); env.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc) throw std::runtime_error("spawn setup: " + std::string(std::strerror(rc)));
  struct Cleanup {
    posix_spawn_file_actions_t *actions;
    ~Cleanup() { posix_spawn_file_actions_destroy(actions); }
  } cleanup{&actions};
  auto log = (work.path / "diagnostics").string();
  rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  if (rc) throw std::runtime_error("spawn redirection: " + std::string(std::strerror(rc)));
  pid_t child;
  rc = posix_spawnp(&child, linker.c_str(), &actions, nullptr, argv.data(), env.data());
  if (rc) throw std::runtime_error("cannot execute ELF linker '" + linker + "': " + std::strerror(rc));
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) throw std::runtime_error("wait for ELF linker failed: " + std::string(std::strerror(errno)));
  }
  std::ifstream input(log, std::ios::binary);
  std::string diagnostics(diagnosticLimit, '\0');
  input.read(diagnostics.data(), diagnostics.size());
  diagnostics.resize(static_cast<size_t>(input.gcount()));
  if (input.peek() != EOF) diagnostics += "\n[linker diagnostics truncated]\n";
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    std::string reason = WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status))
                                           : "exit " + std::to_string(WEXITSTATUS(status));
    throw std::runtime_error("ELF linker '" + linker + "' failed (" + reason + "):\n" + diagnostics);
  }
  return diagnostics;
}

// These options consume an argv token even when that token begins with '-'.
// Keep qld's intercepted output/script options out of this set.
bool takesValue(const std::string& arg) {
  static const std::set<std::string> options = {
    "-l", "--library", "-L", "--library-path", "-e", "--entry", "-m", "--emulation",
    "-z", "-h", "-soname", "--soname", "-I", "-dynamic-linker", "--dynamic-linker",
    "-rpath", "--rpath", "-rpath-link", "--rpath-link", "--version-script",
    "--dynamic-list", "--retain-symbols-file", "--export-dynamic-symbol-list",
    "--export-dynamic-symbol", "--hash-style", "--exclude-libs", "--sysroot",
    "-plugin", "--plugin", "-plugin-opt", "--plugin-opt", "-u", "--undefined",
    "--require-defined", "--defsym", "-defsym", "--wrap", "--section-start",
    "-Ttext", "-Tdata", "-Tbss", "-Ttext-segment", "--oformat", "--format", "-b",
    "--init", "-init", "--fini", "-fini", "--filter", "-F", "--auxiliary", "-f",
    "--sort-section", "--orphan-handling", "--dependency-file", "--out-implib",
    "--compress-debug-sections", "--unresolved-symbols", "--audit", "--depaudit"
  };
  return options.count(arg) != 0;
}

bool extensionScript(const std::string& text, const std::string& path) {
  if (std::filesystem::path(path).extension() == ".lua" || text.starts_with("--!lua")) return true;
  size_t start = 0;
  while (start < text.size()) {
    auto p = text.find_first_not_of(" \t\r", start);
    if (p != text.npos && text[p] == '@') return true;
    auto next = text.find('\n', start);
    if (next == text.npos) break;
    start = next + 1;
  }
  return false;
}
} // namespace
#endif

bool linkElf(LinkState& state, std::vector<uint8_t>& out, std::string& error,
             const ScriptSDK& sdk) {
#ifndef QLD_ENABLE_ELF_BACKEND
  (void)state; (void)out; (void)sdk;
  error = "ELF toolchain backend disabled; rebuild with YELLOWSTONE_ENABLE_ELF_BACKEND=ON";
  return false;
#else
  try {
    Workspace work;
    auto& o = state.options;
    std::string linker = o.elf.linker.empty() ? defaultElfLinker() : o.elf.linker;
    std::vector<std::string> raw;
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
    std::vector<std::string> expanded, args;
    std::set<std::filesystem::path> active;
    size_t budget = sourceLimit;
    expandArguments(raw, expanded, active, budget);
    unsigned scriptNumber = 0;
    auto script = [&](std::string text, const std::string& name) {
      auto path = work.path / ("script" + std::to_string(scriptNumber++) + ".ld");
      writeText(path, sdk.preprocess(text, name));
      return path.string();
    };
    for (size_t i = 0; i < expanded.size(); ++i) {
      const auto& arg = expanded[i];
      auto value = [&]() -> const std::string& {
        if (++i == expanded.size()) throw std::runtime_error("missing argument for " + arg);
        return expanded[i];
      };
      if (arg == "--dynamic") continue;
      if (arg == "--elf-linker") { linker = value(); continue; }
      if (arg.starts_with("--elf-linker=")) { linker = arg.substr(13); continue; }
      if (arg == "-o" || arg == "--output" || arg == "-output") { o.output = value(); continue; }
      if (arg.starts_with("--output=") || arg.starts_with("-output=")) { o.output = arg.substr(arg.find('=') + 1); continue; }
      if (arg.starts_with("-o") && !arg.starts_with("--") && !arg.starts_with("-output") && !arg.starts_with("-orphan") && !arg.starts_with("-oformat")) {
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
        auto text = readText(path, sourceLimit);
        args.push_back("-T");
        args.push_back(extensionScript(text, path) ? script(text, path) : path);
        continue;
      }
      if (arg == "--") {
        while (++i < expanded.size()) args.push_back(std::filesystem::absolute(expanded[i]).string());
        break;
      }
      args.push_back(arg);
      if (takesValue(arg)) args.push_back(value());
    }
    if (linker.empty()) throw std::runtime_error("empty ELF linker executable");
    // Put the controlled output last, including after expanded response files.
    const auto image = work.path / "image.elf";
    args.push_back("-o"); args.push_back(image.string());
    if (!o.mapFile.empty()) { args.push_back("-Map"); args.push_back((work.path / "map").string()); }
    auto diagnostics = runProcess(linker, std::move(args), work);
    auto result = qbfd::open(image.string());
    if (!result || result->object->format() != qbfd::Format::ELF)
      throw std::runtime_error("ELF linker produced no valid ELF image\n" + diagnostics);
    auto bytes = result->buffer->span();
    out.assign(bytes.begin(), bytes.end());
    if (!o.mapFile.empty()) {
      state.mapText = readText(work.path / "map", sourceLimit);
      writeText(o.mapFile, state.mapText);
    }
    if (o.elf.diagnostic && !diagnostics.empty()) o.elf.diagnostic(diagnostics);
    return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
#endif
}
} // namespace qld::detail
