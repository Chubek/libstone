#include "Backend.hpp"
#include "ElfBackend.hpp"
#include "internal.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace qld::detail::toolchain {
std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

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

Workspace::Workspace(const std::filesystem::path& directory) {
#ifdef _WIN32
  // CreateDirectory is exclusive; never adopt an existing temporary directory.
  for (unsigned i = 0; i != 1024; ++i) {
    auto candidate = directory / ("qobjld-" + std::to_string(GetCurrentProcessId()) +
                            "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(i));
    if (CreateDirectoryW(candidate.c_str(), nullptr)) { path = candidate; return; }
    if (GetLastError() != ERROR_ALREADY_EXISTS) break;
  }
  throw std::runtime_error("cannot create linker workspace");
#else
  auto pattern = (directory / "qobjld-XXXXXX").string();
  if (!::mkdtemp(pattern.data()))
    throw std::runtime_error("cannot create linker workspace: " + std::string(std::strerror(errno)));
  path = pattern;
#endif
}
Workspace::~Workspace() { std::error_code ec; std::filesystem::remove_all(path, ec); }

namespace {
std::vector<std::string> words(const std::string& text, Quoting style) {
  std::vector<std::string> result;
  std::string word;
  char quote = 0;
  bool started = false;
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\\' && style == Quoting::GNU) {
      if (++i == text.size()) throw std::runtime_error("trailing escape in response file");
      word += text[i]; started = true;
    } else if (c == '\\' && style == Quoting::Windows) {
      size_t begin = i;
      while (i < text.size() && text[i] == '\\') ++i;
      size_t n = i - begin;
      if (i < text.size() && text[i] == '"') {
        word.append(n / 2, '\\');
        if (n % 2) word += '"';
        else quote = quote ? 0 : '"';
      } else {
        word.append(n, '\\'); --i;
      }
      started = true;
    } else if (quote) {
      if (c == quote) quote = 0; else word += c;
    } else if (c == '"' || (c == '\'' && style == Quoting::GNU)) {
      quote = c; started = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (started) { result.push_back(std::move(word)); word.clear(); started = false; }
    } else { word += c; started = true; }
  }
  if (quote) throw std::runtime_error("unterminated quote in response file");
  if (started) result.push_back(std::move(word));
  return result;
}

void expand(const std::vector<std::string>& args, std::vector<std::string>& out,
            std::set<std::filesystem::path>& active, size_t& budget,
            Quoting style, unsigned depth) {
  if (depth > 64) throw std::runtime_error("response file nesting limit exceeded");
  for (const auto& arg : args) {
    if (arg.size() + 1 > budget) throw std::runtime_error("linker argument size limit exceeded");
    budget -= arg.size() + 1;
    // Mach-O loader tokens and explicit hook/inline-script values are literal.
    if (arg.starts_with('@') && !arg.starts_with("@rpath/") &&
        !arg.starts_with("@loader_path/") && !arg.starts_with("@executable_path/") &&
        arg != "@rpath" && arg != "@loader_path" && arg != "@executable_path" &&
        (out.empty() || (out.back() != "--script-text" && out.back() != "--lto-script"))) {
      auto path = std::filesystem::canonical(arg.substr(1));
      if (!active.insert(path).second) throw std::runtime_error("response file cycle: " + path.string());
      auto text = readText(path, budget);
      expand(words(text, style), out, active, budget, style, depth + 1);
      active.erase(path);
    } else out.push_back(arg);
  }
}

std::string quoted(const std::string& arg, Quoting style) {
  std::string result = "\"";
  size_t slashes = 0;
  for (char c : arg) {
    if (style == Quoting::GNU) {
      if (c == '\\' || c == '"') result += '\\';
      result += c;
    } else if (c == '\\') {
      ++slashes;
    } else {
      result.append(slashes * (c == '"' ? 2 : 1), '\\');
      slashes = 0;
      if (c == '"') result += '\\';
      result += c;
    }
  }
  result.append(slashes * 2, '\\');
  return result + '"';
}
#ifdef _WIN32
std::wstring wide(const std::string& text) {
  int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
  if (!size && !text.empty()) throw std::runtime_error("invalid UTF-8 linker argument");
  std::wstring out(size, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(), size);
  return out;
}
struct Handle {
  HANDLE value;
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
#endif
}

std::vector<std::string> expandArguments(const std::vector<std::string>& args, Quoting style) {
  std::vector<std::string> out;
  std::set<std::filesystem::path> active;
  size_t budget = sourceLimit;
  expand(args, out, active, budget, style, 0);
  return out;
}

std::string runProcess(const std::string& linker, const std::vector<std::string>& args,
                       const Workspace& work, Quoting style, const char *format) {
  if (std::getenv("QLD_TOOLCHAIN_BACKEND_ACTIVE") || std::getenv("QLD_ELF_BACKEND_ACTIVE"))
    throw std::runtime_error("recursive qobjld backend invocation; select a toolchain linker");
  if (linker.empty()) throw std::runtime_error("empty linker executable");
#if defined(__linux__)
  // A qobjld selected as its own explicit bridge is a configuration error even
  // when its child would now reject the native-only request before respawning.
  std::error_code selfError;
  if (std::filesystem::equivalent(linker, "/proc/self/exe", selfError))
    throw std::runtime_error("recursive qobjld backend invocation; select a toolchain linker");
#endif
  std::string response;
  for (const auto& arg : args) response += quoted(arg, style) + "\n";
  auto rsp = work.path / "arguments.rsp";
  auto log = work.path / "diagnostics";
  writeText(rsp, response);
  unsigned code = 0;
  std::string failure;
#ifdef _WIN32
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  Handle output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (output.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open linker diagnostics");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = startup.hStdError = output.value;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  auto cmd = wide(quoted(linker, Quoting::Windows) + " " +
                  quoted("@" + rsp.string(), Quoting::Windows));
  LPWCH rawEnv = GetEnvironmentStringsW();
  if (!rawEnv) throw std::runtime_error("cannot read process environment");
  std::vector<std::wstring> entries;
  for (auto p = rawEnv; *p; p += std::wcslen(p) + 1) entries.emplace_back(p);
  FreeEnvironmentStringsW(rawEnv);
  entries.emplace_back(L"QLD_TOOLCHAIN_BACKEND_ACTIVE=1");
  std::sort(entries.begin(), entries.end());
  std::wstring env;
  for (const auto& entry : entries) { env += entry; env += L'\0'; }
  env += L'\0';
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_UNICODE_ENVIRONMENT,
                      env.data(), nullptr, &startup, &process))
    throw std::runtime_error("cannot execute " + std::string(format) + " linker '" + linker +
                             "' (Windows error " + std::to_string(GetLastError()) + ")");
  Handle child{process.hProcess}, thread{process.hThread};
  if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0)
    throw std::runtime_error("wait for linker failed");
  DWORD status;
  if (!GetExitCodeProcess(child.value, &status)) throw std::runtime_error("cannot read linker exit status");
  code = status;
#else
  std::string responseArg = "@" + rsp.string();
  char *argv[] = {const_cast<char *>(linker.c_str()), responseArg.data(), nullptr};
  std::vector<char *> env;
  for (char **p = environ; *p; ++p) env.push_back(*p);
  char marker[] = "QLD_TOOLCHAIN_BACKEND_ACTIVE=1";
  env.push_back(marker); env.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  int rc = posix_spawn_file_actions_init(&actions);
  if (rc) throw std::runtime_error("spawn setup: " + std::string(std::strerror(rc)));
  struct Cleanup {
    posix_spawn_file_actions_t *actions;
    ~Cleanup() { posix_spawn_file_actions_destroy(actions); }
  } cleanup{&actions};
  rc = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (!rc) rc = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  if (rc) throw std::runtime_error("spawn redirection: " + std::string(std::strerror(rc)));
  pid_t child;
  rc = posix_spawnp(&child, linker.c_str(), &actions, nullptr, argv, env.data());
  if (rc) throw std::runtime_error("cannot execute " + std::string(format) + " linker '" + linker + "': " + std::strerror(rc));
  int status = 0;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) throw std::runtime_error("wait for linker failed: " + std::string(std::strerror(errno)));
  }
  if (WIFSIGNALED(status)) failure = "signal " + std::to_string(WTERMSIG(status));
  else code = unsigned(WEXITSTATUS(status));
#endif
  std::ifstream input(log, std::ios::binary);
  std::string diagnostics(1024 * 1024, '\0');
  input.read(diagnostics.data(), std::streamsize(diagnostics.size()));
  diagnostics.resize(static_cast<size_t>(input.gcount()));
  if (input.peek() != EOF) diagnostics += "\n[linker diagnostics truncated]\n";
  if (code || !failure.empty())
    throw std::runtime_error(std::string(format) + " linker '" + linker + "' failed (" +
                             (failure.empty() ? "exit " + std::to_string(code) : failure) + "):\n" + diagnostics);
  return diagnostics;
}

void publish(const std::filesystem::path& staged, const std::string& destination) {
  if (destination.empty()) return;
  std::error_code ec;
  std::filesystem::copy_file(staged, destination, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) throw std::runtime_error("cannot write " + destination + ": " + ec.message());
}

void checkDestinations(const std::string& image, const std::vector<std::string>& auxiliary) {
  if (std::none_of(auxiliary.begin(), auxiliary.end(), [](const auto& s) { return !s.empty(); })) return;
  std::vector<std::filesystem::path> paths;
  if (!image.empty()) paths.push_back(std::filesystem::weakly_canonical(image));
  for (const auto& name : auxiliary) {
    if (name.empty()) continue;
    auto path = std::filesystem::weakly_canonical(name);
    for (const auto& previous : paths) {
      std::error_code ec;
      if (path == previous || std::filesystem::equivalent(path, previous, ec))
        throw std::runtime_error("linker output paths collide: " + name);
    }
    paths.push_back(std::move(path));
  }
}

void rejectIgnoredOptions(const std::string& diagnostics) {
  if (diagnostics.find("ignoring unknown argument") != std::string::npos ||
      diagnostics.find("not yet implemented") != std::string::npos)
    throw std::runtime_error("toolchain linker ignored an unsupported option:\n" + diagnostics);
}

unsigned optionArity(const std::string& arg) {
  static const std::set<std::string> one = {
    "-o", "--output", "-output", "-e", "--entry", "-L", "--library-path", "-l", "--library",
    "-m", "--emulation", "-T", "--script", "--script-text", "--lto-script", "-Map", "--Map", "-map",
    "--image-base", "--format", "--pe-linker", "--macho-linker", "--elf-linker",
    "-arch", "-install_name", "-dylib_install_name", "-dylinker_install_name", "-final_output",
    "-compatibility_version", "-current_version", "-macos_version_min", "-syslibroot",
    "-rpath", "-F", "-framework", "-needed_framework", "-weak_framework", "-reexport_framework",
    "-force_load", "-load_hidden", "-needed_library", "-weak_library", "-reexport_library",
    "-bundle_loader", "-filelist", "-undefined", "-u", "-U", "-exported_symbol", "-unexported_symbol",
    "-exported_symbols_list", "-unexported_symbols_list", "-order_file", "-pagezero_size",
    "-seg1addr", "-image_base", "-headerpad", "-dependency_info", "-object_path_lto", "-oso_prefix",
    "-z", "--soname", "-soname", "--dynamic-linker", "-dynamic-linker", "--version-script",
    "--rpath", "--rpath-link", "-rpath-link", "--sysroot", "-plugin", "-plugin-opt", "-b",
    "--oformat", "--defsym", "--wrap", "--out-implib", "--subsystem", "--export",
    "-add_ast_path", "-cache_path_lto", "-client_name", "-dyld_env", "-lto_library",
    "-max_relative_cache_size_lto", "-mcpu", "-mllvm", "-non_global_symbols_no_strip_list",
    "-non_global_symbols_strip_list", "-prune_after_lto", "-prune_interval_lto",
    "-sub_library", "-sub_umbrella", "-umbrella", "-allowable_client", "-dylib_file",
    "-seg_addr_table", "-segalign",
    "-h", "-I", "--dynamic-list", "--retain-symbols-file", "--export-dynamic-symbol-list",
    "--export-dynamic-symbol", "--hash-style", "--exclude-libs", "--plugin", "--plugin-opt",
    "--undefined", "--require-defined", "-defsym", "--section-start", "-Ttext", "-Tdata",
    "-Tbss", "-Ttext-segment", "--init", "-init", "--fini", "-fini", "--filter",
    "--auxiliary", "-f", "--sort-section", "--orphan-handling", "--dependency-file",
    "--compress-debug-sections", "--unresolved-symbols", "--audit", "--depaudit"
  };
  if (one.count(arg)) return 1;
  if (arg == "-platform_version" || arg == "-sectcreate" || arg == "-segcreate" ||
      arg == "-sectalign" || arg == "-segprot" || arg == "-sectorder") return 3;
  if (arg == "-alias" || arg == "-add_empty_section" || arg == "-rename_segment" ||
      arg == "-segaddr" || arg == "-seg_page_size") return 2;
  if (arg == "-rename_section") return 4;
  return 0;
}
} // namespace qld::detail::toolchain

namespace qld::detail {
namespace {
Backend formatBackend(std::span<const uint8_t> bytes) {
  switch (qbfd::detectFormat(bytes)) {
    case qbfd::Format::PE: case qbfd::Format::COFF: return Backend::PE;
    case qbfd::Format::MachO: return Backend::MachO;
    case qbfd::Format::ELF: return Backend::ELF;
    default: break;
  }
  // COFF short import objects (distinct from bigobj).
  if (bytes.size() >= 20 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 255 && bytes[3] == 255)
    return Backend::PE;
  return Backend::Auto;
}
Backend pathBackend(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::array<uint8_t, 64> head{};
  in.read(reinterpret_cast<char *>(head.data()), head.size());
  auto bytes = std::span(head.data(), size_t(in.gcount()));
  if (!qbfd::isArchive(bytes)) return formatBackend(bytes);
  auto archive = qbfd::openArchive(path);
  if (!archive) return Backend::Auto; // The selected linker diagnoses malformed archives.
  for (const auto& member : archive->archive->members()) {
    if (member.special) continue;
    auto payload = archive->archive->readMember(member);
    if (payload) {
      auto kind = formatBackend(*payload);
      if (kind != Backend::Auto) return kind;
    }
  }
  return Backend::Auto;
}
} // namespace
Backend namedBackend(const std::string& name) {
  auto n = toolchain::lower(name);
  if (n == "elf") return Backend::ELF;
  if (n == "pe" || n == "coff" || n == "pe-coff") return Backend::PE;
  if (n == "macho" || n == "mach-o") return Backend::MachO;
  if (n == "auto") return Backend::Auto;
  throw std::runtime_error("unknown linker format: " + name);
}

Backend inputBackend(const std::vector<std::string>& paths) {
  Backend selected = Backend::Auto;
  for (const auto& path : paths) {
    auto kind = pathBackend(path);
    if (kind == Backend::Auto) continue;
    if (selected != Backend::Auto && selected != kind)
      throw std::runtime_error(path + ": mixed object formats in link");
    selected = kind;
  }
  return selected;
}

Backend selectBackend(const LinkOptions& o) {
  if (unsigned(o.elf.enabled) + unsigned(o.pe.enabled) + unsigned(o.macho.enabled) > 1)
    throw std::runtime_error("select only one linker backend");
  if (o.pe.enabled) return Backend::PE;
  if (o.macho.enabled) return Backend::MachO;
  if (o.elf.enabled) return Backend::ELF;
  auto selected = inputBackend(o.inputs);
  if (selected == Backend::PE || selected == Backend::MachO) return selected;
  return Backend::Auto;
}

Backend invocationBackend(const std::vector<std::string>& raw) {
  Backend explicitKind = Backend::Auto, hint = Backend::Auto;
  auto scan = [&](const std::vector<std::string>& args, bool files) {
    std::vector<std::string> paths;
    for (size_t i = 0; i < args.size(); ++i) {
      const auto& a = args[i];
      if (a == "--") {
        paths.insert(paths.end(), args.begin() + i + 1, args.end());
        break;
      }
      Backend kind = Backend::Auto;
      if (a.starts_with("--format=")) kind = namedBackend(a.substr(9));
      else if (a == "--format") {
        if (++i == args.size()) throw std::runtime_error("missing argument for --format");
        kind = namedBackend(args[i]);
      } else if (a == "--pe-linker" || a.starts_with("--pe-linker=")) kind = Backend::PE;
      else if (a == "--macho-linker" || a.starts_with("--macho-linker=")) kind = Backend::MachO;
      else if (a == "--elf-linker" || a.starts_with("--elf-linker=")) kind = Backend::ELF;
      if (kind != Backend::Auto) {
        if (explicitKind != Backend::Auto && explicitKind != kind)
          throw std::runtime_error("conflicting linker formats");
        explicitKind = kind;
      }
      auto low = toolchain::lower(a);
      if (low == "/dll" || low.starts_with("/machine:") || low.starts_with("/out:")) hint = Backend::PE;
      if (a == "-arch" || a == "-platform_version" || a == "-dylib" || a == "-bundle") hint = Backend::MachO;
      if (a == "-m" && i + 1 < args.size() && (args[i + 1] == "i386pep" || args[i + 1] == "i386pe"))
        hint = Backend::PE;
      if (a != "--format") i += toolchain::optionArity(a);
      if (files && !a.empty() && a[0] != '-' && a[0] != '@') paths.push_back(a);
    }
    return files ? inputBackend(paths) : Backend::Auto;
  };
  scan(raw, false);
  auto style = explicitKind == Backend::PE || hint == Backend::PE
                   ? toolchain::Quoting::Windows : toolchain::Quoting::GNU;
  auto expanded = toolchain::expandArguments(raw, style);
  auto inputs = scan(expanded, true);
  if (explicitKind != Backend::Auto) return explicitKind;
  if (inputs != Backend::Auto) return inputs;
  return hint;
}
} // namespace qld::detail
