#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "cephyr/Cephyr.h"
#include "cephyr/backend.hpp"

#include "cephyr/cache.hpp"
#include "cephyr/config.hpp"
#include "cephyr/diagnostics.hpp"
#include "cephyr/host_target.hpp"
#include "cephyr/process.hpp"
#include "cephyr/target.hpp"

#include <Backend.h>
#include <archon/Archon-IRGen.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

unsigned cephyr_python_version(void) {
  return (PY_MAJOR_VERSION << 16) | PY_MINOR_VERSION;
}

namespace cephyr {
namespace {

constexpr std::string_view kCacheFormat = "cephyr-archon-native-v1";
constexpr std::string_view kCacheFormatFile = "__cephyr.backend.format";
constexpr std::string_view kCacheFingerprintFile =
    "__cephyr.backend.fingerprint";
constexpr std::string_view kCacheContentsFile = "__cephyr.backend.contents";

constexpr std::array<std::string_view, 19> kRV64Sources = {
    "ABI.c",
    "Backend.c",
    "CFGLowering.c",
    "FrameLowering.c",
    "IntegerConversion.c",
    "Legalizer.c",
    "MachineIR.c",
    "MIRText.c",
    "NativeEmitter.c",
    "NativeEncoding.c",
    "NativeFrameVerify.c",
    "NativeHomes.c",
    "NativeISel.c",
    "NativeLayout.c",
    "NativeRegalloc.c",
    "NativeSchedule.c",
    "NativeVerify.c",
    "NativeABIData.c",
    "NativeEncodingData.c",
};

constexpr std::array<std::string_view, 7>
scalar_gas_sources(std::string_view emitter) {
  return {"Backend.c", "Legalizer.c", "MIRText.c", "NativeVerify.c",
          "ScalarGAS.c", emitter, "NativeABIData.c"};
}

constexpr auto kAMD64Sources = scalar_gas_sources("AMD64GAS.c");
constexpr auto kAArch64Sources = scalar_gas_sources("AArch64GAS.c");

std::span<const std::string_view> native_sources(std::string_view architecture) {
  if (architecture == "AMD64")
    return kAMD64Sources;
  if (architecture == "AArch64")
    return kAArch64Sources;
  return kRV64Sources;
}

class Sha256 {
public:
  void update(const void *raw, std::size_t size) {
    const auto *data = static_cast<const std::uint8_t *>(raw);
    total_ += static_cast<std::uint64_t>(size);
    while (size) {
      const auto count = std::min(size, block_.size() - used_);
      std::memcpy(block_.data() + used_, data, count);
      used_ += count;
      data += count;
      size -= count;
      if (used_ == block_.size()) {
        transform(block_.data());
        used_ = 0;
      }
    }
  }

  void update(std::string_view text) { update(text.data(), text.size()); }

  std::string finish() {
    const std::uint64_t bits = total_ * 8;
    const std::uint8_t marker = 0x80;
    update(&marker, 1);
    const std::uint8_t zero = 0;
    while (used_ != 56)
      update(&zero, 1);
    std::array<std::uint8_t, 8> length{};
    for (unsigned i = 0; i != length.size(); ++i)
      length[length.size() - i - 1] =
          static_cast<std::uint8_t>(bits >> (i * 8));
    update(length.data(), length.size());

    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (const auto word : state_)
      result << std::setw(8) << word;
    return result.str();
  }

private:
  static constexpr std::array<std::uint32_t, 64> round_ = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };

  static std::uint32_t rotate(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32 - amount));
  }

  void transform(const std::uint8_t *data) {
    std::array<std::uint32_t, 64> words{};
    for (unsigned i = 0; i != 16; ++i) {
      const unsigned offset = i * 4;
      words[i] = (static_cast<std::uint32_t>(data[offset]) << 24) |
                 (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
                 (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
                 static_cast<std::uint32_t>(data[offset + 3]);
    }
    for (unsigned i = 16; i != words.size(); ++i) {
      const auto a = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^
                     (words[i - 15] >> 3);
      const auto b = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^
                     (words[i - 2] >> 10);
      words[i] = words[i - 16] + a + words[i - 7] + b;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (unsigned i = 0; i != words.size(); ++i) {
      const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto first = h + s1 + choose + round_[i] + words[i];
      const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto second = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_ = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  std::array<std::uint8_t, 64> block_{};
  std::uint64_t total_ = 0;
  std::size_t used_ = 0;
};

void hash_field(Sha256 &hash, std::string_view name, std::string_view value) {
  hash.update(name);
  const std::uint8_t separator = 0;
  hash.update(&separator, 1);
  const std::uint64_t size = value.size();
  hash.update(&size, sizeof(size));
  hash.update(value);
}

bool read_file(const std::filesystem::path &path, std::string &output,
               std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "cannot read " + path.string();
    return false;
  }
  output.assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
  if (input.bad()) {
    error = "failed while reading " + path.string();
    return false;
  }
  return true;
}

bool hash_file(Sha256 &hash, std::string_view label,
               const std::filesystem::path &path, std::string &error) {
  std::string contents;
  if (!read_file(path, contents, error))
    return false;
  hash_field(hash, label, contents);
  return true;
}

bool is_generated_resource(const std::filesystem::path &path) {
  for (const auto &part : path) {
    if (part == "__pycache__" || part == ".pytest_cache")
      return false;
  }
  const auto extension = path.extension().string();
  return extension != ".pyc" && extension != ".pyo";
}

bool hash_tree(Sha256 &hash, const std::filesystem::path &root,
               std::string_view label, std::string &error) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec) {
    error = "Archon resource directory is unavailable: " + root.string();
    return false;
  }
  std::vector<std::filesystem::path> files;
  std::filesystem::recursive_directory_iterator iterator(root, ec), end;
  if (ec) {
    error = "cannot enumerate Archon resources in " + root.string() + ": " +
            ec.message();
    return false;
  }
  while (iterator != end) {
    const auto entry = *iterator;
    const auto relative = entry.path().lexically_relative(root);
    if (entry.is_symlink(ec)) {
      if (ec) {
        error = "cannot inspect Archon resource " + entry.path().string() +
                ": " + ec.message();
      } else {
        error = "refusing symlinked Archon resource " + entry.path().string();
      }
      return false;
    }
    if (entry.is_directory(ec)) {
      if (ec) {
        error = "cannot inspect Archon resource " + entry.path().string() +
                ": " + ec.message();
        return false;
      }
      if (!is_generated_resource(relative))
        iterator.disable_recursion_pending();
    } else if (entry.is_regular_file(ec)) {
      if (ec) {
        error = "cannot inspect Archon resource " + entry.path().string() +
                ": " + ec.message();
        return false;
      }
      if (is_generated_resource(relative))
        files.push_back(relative);
    } else {
      error = "unsupported Archon resource file type: " + entry.path().string();
      return false;
    }
    iterator.increment(ec);
    if (ec) {
      error = "cannot enumerate Archon resources in " + root.string() + ": " +
              ec.message();
      return false;
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto &relative : files) {
    const auto name = std::string(label) + "/" + relative.generic_string();
    if (!hash_file(hash, name, root / relative, error))
      return false;
  }
  return true;
}

std::optional<std::filesystem::path> find_executable(std::string_view command) {
  if (command.empty())
    return std::nullopt;
  std::filesystem::path name(command);
  std::error_code ec;
  if (name.has_parent_path()) {
    const auto canonical = std::filesystem::weakly_canonical(name, ec);
    if (!ec && std::filesystem::is_regular_file(canonical, ec) && !ec)
      return canonical;
    return std::nullopt;
  }
  const char *raw_path = std::getenv("PATH");
  if (!raw_path)
    return std::nullopt;
  std::string_view path(raw_path);
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const auto end = path.find(':', begin);
    const auto component =
        path.substr(begin, end == std::string_view::npos ? path.size() - begin
                                                         : end - begin);
    const auto candidate =
        std::filesystem::path(component.empty() ? "."
                                                : std::string(component)) /
        name;
    const auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (!ec && std::filesystem::is_regular_file(canonical, ec) && !ec)
      return canonical;
    ec.clear();
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return std::nullopt;
}

const char *stage_name(ArchonBackendStage stage) {
  switch (stage) {
  case ARCHON_BACKEND_INPUT:
    return "input";
  case ARCHON_BACKEND_VERIFY:
    return "verification";
  case ARCHON_BACKEND_SELECT:
    return "instruction selection";
  case ARCHON_BACKEND_ALLOCATE:
    return "register allocation";
  case ARCHON_BACKEND_EMIT:
    return "assembly emission";
  case ARCHON_BACKEND_SCHEDULE:
    return "scheduling";
  }
  return "unknown stage";
}

std::string content_digest(std::string_view contents) {
  Sha256 hash;
  hash.update(contents);
  return hash.finish();
}

using Artifacts = std::map<std::string, std::string>;

class PythonError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

void ensure_python() {
  // Hold the lock only for the initialization check. Never acquire the GIL
  // with this lock held once a fully initialized interpreter exists.
  static std::mutex mutex;
  static bool attempted = false;
  std::lock_guard lock(mutex);
  if (Py_IsInitialized()) {
    attempted = true;
    return;
  }
  if (attempted)
    throw PythonError("Cephyr cannot restart a previously initialized CPython runtime");
  // A plain native host may have loaded libcephyr with RTLD_LOCAL. CPython's
  // shared extension modules still need to resolve symbols from libpython.
  // Pin and promote libpython when Cephyr starts the interpreter, so it also
  // survives a later dlclose of libcephyr.
  const auto anchor = reinterpret_cast<const void *>(&Py_IsInitialized);
  Dl_info python_library{};
  if (!dladdr(anchor, &python_library) || !python_library.dli_fname ||
      (!dlopen(python_library.dli_fname, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD) &&
       dlsym(RTLD_DEFAULT, "Py_IsInitialized") != anchor))
    throw PythonError("cannot make the CPython runtime available to its extension modules");
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  config.install_signal_handlers = 0;
  config.site_import = 0;
  config.write_bytecode = 0;
  attempted = true;
  const auto status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status))
    throw PythonError(status.err_msg ? status.err_msg : "cannot initialize CPython");
  // The first native caller may leave or be a different thread from future
  // callers. Do not finalize or reinitialize this process-wide interpreter.
  PyEval_SaveThread();
}

class PythonScope {
public:
  PythonScope() : state_(PyGILState_Ensure()) {
    PyErr_Fetch(&type_, &value_, &traceback_);
  }
  ~PythonScope() {
    PyErr_Restore(type_, value_, traceback_);
    PyGILState_Release(state_);
  }
  PythonScope(const PythonScope &) = delete;
  PythonScope &operator=(const PythonScope &) = delete;

private:
  PyGILState_STATE state_;
  PyObject *type_, *value_, *traceback_;
};

struct PyDecref {
  void operator()(PyObject *object) const noexcept { Py_DECREF(object); }
};
using PyRef = std::unique_ptr<PyObject, PyDecref>;

[[noreturn]] void python_error() {
  PyObject *type, *value, *traceback;
  PyErr_Fetch(&type, &value, &traceback);
  PyErr_NormalizeException(&type, &value, &traceback);
  const PyRef owned_type(type), owned_value(value), owned_traceback(traceback);
  const PyRef text(value ? PyObject_Str(value) : nullptr);
  const char *message = text ? PyUnicode_AsUTF8(text.get()) : nullptr;
  PyErr_Clear();
  throw PythonError(message ? message : "Archon CPython operation failed");
}

PyRef checked(PyObject *object) {
  if (!object)
    python_error();
  return PyRef(object);
}

class ArchonPython {
public:
  struct Database {
    archon::Value value;
    std::string text, architecture;
  };

  ArchonPython(const std::filesystem::path &root, std::string_view package_digest)
      : root_(root.string()) {
    ensure_python();
    Sha256 hash;
    hash_field(hash, "root", root_);
    hash_field(hash, "package", package_digest);
    name_ = "_cephyr_archon_" + hash.finish();
  }

  Database compile_database(const std::filesystem::path &specification) const {
    PythonScope scope;
    ensure_namespace();
    const archon::PackageScope package(name_);
    Database db;
    db.value = archon::compile_file(specification.string());
    db.text = archon::dump_database(db.value);
    db.architecture = db.value.at("architecture").as_string();
    return db;
  }

  Artifacts generate(const Config &config, const Target &target,
                     const archon::Value &database) const {
    PythonScope scope;
    ensure_namespace();
    const archon::PackageScope package(name_);
    const archon::Object options{
        {"profile", "native"}, {"target", target.architecture},
        {"abi", target.abi}, {"oracle", false},
    };
    return archon::irgen::generate_ir(
        config.representation.empty() ? "gated" : config.representation,
        {database}, options);
  }

  void publish(const Artifacts &files, const std::filesystem::path &directory,
               const std::filesystem::path &specification) const {
    PythonScope scope;
    ensure_namespace();
    const archon::PackageScope package(name_);
    archon::irgen::write_generated_directory(files, directory.string(),
                                             {specification.string()}, true);
  }

private:
  void ensure_namespace() const {
    // A private parent package lets the normal importer load and synchronize
    // this Archon copy, including its relative imports. Never prepend to
    // sys.path or replace the host's own `archon` package.
    auto *modules = PyImport_GetModuleDict();
    if (!PyDict_GetItemString(modules, name_.c_str())) {
      const auto parent = checked(PyModule_New(name_.c_str()));
      const auto location = checked(PyUnicode_DecodeFSDefault(root_.c_str()));
      const auto paths = checked(Py_BuildValue("[O]", location.get()));
      if (PyObject_SetAttrString(parent.get(), "__path__", paths.get()) < 0 ||
          PyDict_SetItemString(modules, name_.c_str(), parent.get()) < 0)
        python_error();
    }
  }

  std::string root_, name_;
};

// LMDB process-level file locks do not support independently opening the same
// environment in several threads. Keep cache lifetimes short and never hold
// this lock while calling Python or external tools.
std::mutex &backend_cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::string cache_contents(const std::map<std::string, std::string> &files) {
  std::string result;
  for (const auto &[name, contents] : files) {
    if (name == kCacheFormatFile || name == kCacheFingerprintFile ||
        name == kCacheContentsFile)
      continue;
    result += name;
    result.push_back('\t');
    result += content_digest(contents);
    result.push_back('\n');
  }
  return result;
}

bool cache_metadata(const std::map<std::string, std::string> &files,
                    std::string_view fingerprint,
                    std::span<const std::string_view> sources) {
  const auto format = files.find(std::string(kCacheFormatFile));
  const auto stored_fingerprint =
      files.find(std::string(kCacheFingerprintFile));
  const auto contents = files.find(std::string(kCacheContentsFile));
  if (format == files.end() || format->second != kCacheFormat ||
      stored_fingerprint == files.end() ||
      stored_fingerprint->second != fingerprint || contents == files.end() ||
      contents->second != cache_contents(files))
    return false;
  for (const auto source : sources) {
    if (files.find(std::string(source)) == files.end())
      return false;
  }
  return files.find("Backend.h") != files.end() &&
         files.find("BackendManifest.json") != files.end();
}

Artifacts generated_files(const Artifacts &cache_files) {
  Artifacts result;
  for (const auto &[name, contents] : cache_files) {
    if (name != kCacheFormatFile && name != kCacheFingerprintFile &&
        name != kCacheContentsFile)
      result.emplace(name, contents);
  }
  return result;
}

} // namespace

struct Backend::Impl {
  using CompileText = ArchonBackendStatus (*)(const char *, std::size_t,
                                              const ArchonBackendOptions *,
                                              unsigned, ArchonBackendArtifact *,
                                              ArchonBackendDiagnostic *);
  using DestroyArtifact = void (*)(ArchonBackendArtifact *);
  using BackendTarget = const char *(*)();

  const Config &config;
  const Target &target;
  std::filesystem::path workspace;
  Diagnostics &diagnostics;

  bool attempted = false;
  bool ready = false;
  void *library = nullptr;
  CompileText compile_text = nullptr;
  DestroyArtifact destroy_artifact = nullptr;
  BackendTarget backend_target = nullptr;
  std::string fingerprint;
  std::string database_text;
  std::string database_architecture;

  Impl(const Config &config, const Target &target,
       const std::filesystem::path &workspace, Diagnostics &diagnostics)
      : config(config), target(target), workspace(workspace),
        diagnostics(diagnostics) {}

  ~Impl() {
    if (library)
      dlclose(library);
  }

  Location location(const std::string &file) const { return Location{file}; }

  void error(std::string_view code, std::string message,
             const std::string &file = {}) {
    diagnostics.error(code, std::move(message), location(file));
  }

  bool read_package_digest(const std::filesystem::path &root, std::string &digest) {
    Sha256 hash;
    std::string failure;
    if (!hash_tree(hash, root / "archon", "archon", failure)) {
      error("cephyr.backend.resources", failure, target.specification.string());
      return false;
    }
    digest = hash.finish();
    return true;
  }

  bool make_fingerprint(std::string &result, const std::filesystem::path &root,
                        std::string_view package_digest) {
    Sha256 hash;
    hash_field(hash, "format", kCacheFormat);
    hash_field(hash, "canonical-archdb", database_text);
    hash_field(hash, "representation",
               config.representation.empty() ? "gated" : config.representation);
    hash_field(hash, "profile", "native");
    hash_field(hash, "target", database_architecture);
    hash_field(hash, "configured-target", target.architecture);
    hash_field(hash, "abi", target.abi);
    hash_field(hash, "triple", target.triple);
    hash_field(hash, "oracle", "false");
    hash_field(hash, "backend-abi", std::to_string(ARCHON_BACKEND_ABI_VERSION));
    hash_field(hash, "mir-schema", std::to_string(ARCHON_MIR_SCHEMA_VERSION));
    for (std::size_t i = 0; i != config.host_cc.size(); ++i)
      hash_field(hash, "host-cc-" + std::to_string(i), config.host_cc[i]);
    for (std::size_t i = 0; i != config.compile_flags.size(); ++i)
      hash_field(hash, "compile-flag-" + std::to_string(i),
                 config.compile_flags[i]);

    std::string failure;
    hash_field(hash, "archon-package", package_digest);

    // Full generate_ir embeds these resources from adjacent cornerstone
    // projects. Hashing them makes a cache entry invalid as soon as any
    // deterministic generator input changes.
    const auto cornerstone = root.parent_path();
    const std::array<std::pair<std::string_view, std::filesystem::path>, 3>
        external = {{
            {"ASDL/scripts/asdl.pl",
             cornerstone / "ASDL" / "scripts" / "asdl.pl"},
            {"Aurocks/scripts/aurocks.pl",
             cornerstone / "Aurocks" / "scripts" / "aurocks.pl"},
            {"LICENSE", cornerstone.parent_path() / "LICENSE"},
        }};
    for (const auto &[name, path] : external) {
      if (!hash_file(hash, name, path, failure)) {
        error("cephyr.backend.resources", failure,
              target.specification.string());
        return false;
      }
    }

    const auto compiler = find_executable(config.host_cc.front());
    if (compiler) {
      hash_field(hash, "host-cc-path", compiler->string());
      if (!hash_file(hash, "host-cc-content", *compiler, failure)) {
        error("cephyr.backend.host-cc", failure);
        return false;
      }
    } else {
      const char *path = std::getenv("PATH");
      hash_field(hash, "host-cc-unresolved-path", path ? path : "");
    }
    result = hash.finish();
    return true;
  }

  bool validate_configuration() {
    if (config.archon_root.empty()) {
      error("cephyr.backend.config", "Archon package root is not configured");
      return false;
    }
    if (target.specification.empty()) {
      error("cephyr.backend.config",
            "target architecture specification is not configured");
      return false;
    }
    if (!native_lowering_provider(target.architecture, target.abi)) {
      error("cephyr.backend.target",
            "unsupported Cephyr native C lowering provider: " +
                target.architecture + "/" + target.abi,
            target.specification.string());
      return false;
    }
    if (config.host_cc.empty() || config.host_cc.front().empty()) {
      error("cephyr.backend.host-cc", "host C compiler is not configured");
      return false;
    }
    return true;
  }

  bool unchanged(const ArchonPython &python, const std::filesystem::path &root) {
    const auto current = python.compile_database(target.specification);
    if (current.text != database_text) {
      error("cephyr.backend.changed",
            "architecture sources changed during native backend preparation; "
            "retry the Cephyr invocation",
            target.specification.string());
      return false;
    }
    std::string current_fingerprint;
    std::string package_digest;
    if (!read_package_digest(root, package_digest) ||
        !make_fingerprint(current_fingerprint, root, package_digest))
      return false;
    if (current_fingerprint != fingerprint) {
      error("cephyr.backend.changed",
            "Archon generator or host compiler resources changed during "
            "native backend preparation; retry the Cephyr invocation",
            target.specification.string());
      return false;
    }
    return true;
  }

  bool build_library(const std::filesystem::path &generated,
                     const std::filesystem::path &output) {
    std::error_code ec;
    std::filesystem::remove(output, ec);
    std::vector<std::string> command;
    const auto sources = native_sources(target.architecture);
    command.reserve(8 + config.compile_flags.size() + sources.size());
    command.insert(command.end(), config.host_cc.begin(), config.host_cc.end());
    command.emplace_back("-std=c99");
    command.emplace_back("-fPIC");
    command.emplace_back("-shared");
    command.insert(command.end(), config.compile_flags.begin(),
                   config.compile_flags.end());
    command.emplace_back("-I");
    command.push_back(generated.string());
    for (const auto source : sources)
      command.push_back((generated / source).string());
    command.emplace_back("-o");
    command.push_back(output.string());
    const int status = run_process(command, diagnostics, workspace);
    if (status != 0) {
      error("cephyr.backend.build",
            "host C compiler failed while building the generated Archon "
            "backend (exit " +
                std::to_string(status) + ")");
      return false;
    }
    if (!std::filesystem::is_regular_file(output)) {
      error("cephyr.backend.build",
            "host C compiler did not produce the Archon shared library");
      return false;
    }
    return true;
  }

  template <typename Function> bool symbol(Function &output, const char *name) {
    dlerror();
    void *address = dlsym(library, name);
    const char *failure = dlerror();
    if (failure || !address) {
      error("cephyr.backend.load",
            "generated Archon backend is missing " + std::string(name) +
                (failure ? ": " + std::string(failure) : ""));
      return false;
    }
    output = reinterpret_cast<Function>(address);
    return true;
  }

  bool load_library(const std::filesystem::path &path) {
    library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      const char *failure = dlerror();
      error("cephyr.backend.load",
            "cannot load generated Archon backend " + path.string() +
                (failure ? ": " + std::string(failure) : ""));
      return false;
    }
    if (!symbol(compile_text, "archon_backend_compile_text_ex") ||
        !symbol(destroy_artifact, "archon_backend_artifact_destroy") ||
        !symbol(backend_target, "archon_backend_target")) {
      dlclose(library);
      library = nullptr;
      return false;
    }
    const char *actual = backend_target();
    const std::string expected = database_architecture + "/" + target.abi;
    if (!actual || actual != expected) {
      error("cephyr.backend.abi",
            "generated Archon backend target mismatch (expected " + expected +
                ", got " + (actual ? actual : "(null)") + ")");
      dlclose(library);
      library = nullptr;
      compile_text = nullptr;
      destroy_artifact = nullptr;
      backend_target = nullptr;
      return false;
    }
    return true;
  }

  bool prepare_impl() {
    if (!validate_configuration())
      return false;

    std::error_code ec;
    std::filesystem::create_directories(workspace, ec);
    if (ec) {
      error("cephyr.backend.workspace", "cannot create backend workspace " +
                                            workspace.string() + ": " +
                                            ec.message());
      return false;
    }
    workspace = std::filesystem::weakly_canonical(workspace, ec);
    if (ec) {
      error("cephyr.backend.workspace",
            "cannot resolve backend workspace: " + ec.message());
      return false;
    }

    const auto root = std::filesystem::weakly_canonical(config.archon_root);
    std::string package_digest;
    if (!read_package_digest(root, package_digest))
      return false;
    const ArchonPython python(root, package_digest);
    auto db = python.compile_database(target.specification);
    database_text = std::move(db.text);
    database_architecture = std::move(db.architecture);
    if (target.architecture != database_architecture) {
      error("cephyr.backend.target",
            "configured target architecture " + target.architecture +
                " does not match compiled ArchDB " + database_architecture,
            target.specification.string());
      return false;
    }
    if (!make_fingerprint(fingerprint, root, package_digest))
      return false;

    const std::string cache_key = "archon-native:" + fingerprint;
    std::optional<Artifacts> cached;
    if (config.cache_enabled) {
      {
        std::lock_guard lock(backend_cache_mutex());
        Cache cache(config.cache_path, diagnostics);
        cached = cache.get(cache_key);
      }
      if (cached && !cache_metadata(*cached, fingerprint, native_sources(target.architecture))) {
        diagnostics.warning(
            "cephyr.backend.cache",
            "ignoring incomplete or corrupt Archon backend cache entry",
            location(target.specification.string()));
        cached.reset();
      }
    }

    Artifacts generated;
    if (cached) {
      generated = generated_files(*cached);
      if (config.verbose)
        diagnostics.note("cephyr.backend.irgen", "Archon IRGen cache hit");
    } else {
      generated = python.generate(config, target, db.value);
      if (config.verbose)
        diagnostics.note("cephyr.backend.irgen", "Archon IRGen generated");
      for (const auto source : native_sources(target.architecture)) {
        if (!generated.count(std::string(source))) {
          error("cephyr.backend.generate",
                "Archon generation omitted required native source " +
                    std::string(source),
                target.specification.string());
          return false;
        }
      }
    }

    if (!unchanged(python, root))
      return false;

    const auto generated_directory = workspace / "archon-generated";
    python.publish(generated, generated_directory, target.specification);
    const auto library_path = workspace / "libcephyr-archon-backend.so";
    if (!build_library(generated_directory, library_path)) {
      return false;
    }

    if (!load_library(library_path))
      return false;

    if (config.cache_enabled && !cached) {
      generated.emplace(std::string(kCacheFormatFile), std::string(kCacheFormat));
      generated.emplace(std::string(kCacheFingerprintFile), fingerprint);
      generated.emplace(std::string(kCacheContentsFile), cache_contents(generated));
      std::lock_guard lock(backend_cache_mutex());
      Cache cache(config.cache_path, diagnostics);
      cache.put(cache_key, generated);
    }
    return true;
  }

  bool prepare() {
    if (attempted)
      return ready;
    attempted = true;
    try {
      ready = prepare_impl();
    } catch (const archon::Error &failure) {
      error("cephyr.backend.archon", failure.what(),
            target.specification.string());
    } catch (const PythonError &failure) {
      error("cephyr.backend.archon", failure.what(),
            target.specification.string());
    } catch (const std::filesystem::filesystem_error &failure) {
      error("cephyr.backend.io", failure.what());
    } catch (const std::exception &failure) {
      error("cephyr.backend.internal", failure.what());
    }
    return ready;
  }

  std::optional<std::string> compile(std::string_view mir,
                                     const std::string &input_name,
                                     bool position_independent) {
    if (!prepare())
      return std::nullopt;
    ArchonBackendArtifact artifact{};
    ArchonBackendDiagnostic diagnostic{};
    ArchonBackendOptions options{};
    const unsigned flags =
        position_independent ? static_cast<unsigned>(ARCHON_BACKEND_PIC) : 0u;
    const auto status = compile_text(mir.data(), mir.size(), &options, flags,
                                     &artifact, &diagnostic);
    struct ArtifactGuard {
      DestroyArtifact destroy;
      ArchonBackendArtifact *artifact;
      ~ArtifactGuard() { destroy(artifact); }
    } guard{destroy_artifact, &artifact};
    if (status != ARCHON_BACKEND_OK) {
      std::string message =
          "Archon " + std::string(stage_name(diagnostic.stage)) + " failed";
      if (diagnostic.message[0]) {
        message += ": ";
        message += diagnostic.message;
      }
      if (diagnostic.function != std::numeric_limits<std::size_t>::max()) {
        message += " (function " + std::to_string(diagnostic.function);
        if (diagnostic.block != std::numeric_limits<std::size_t>::max())
          message += ", block " + std::to_string(diagnostic.block);
        if (diagnostic.instruction != std::numeric_limits<std::size_t>::max())
          message += ", instruction " + std::to_string(diagnostic.instruction);
        message += ")";
      }
      error("cephyr.backend.mir", std::move(message), input_name);
      return std::nullopt;
    }
    if (!artifact.data) {
      error("cephyr.backend.mir",
            "Archon backend returned success without an assembly artifact",
            input_name);
      return std::nullopt;
    }
    return std::string(artifact.data, artifact.size);
  }
};

Backend::Backend(const Config &config, const Target &target,
                 const std::filesystem::path &workspace,
                 Diagnostics &diagnostics)
    : impl_(std::make_unique<Impl>(config, target, workspace, diagnostics)) {}

Backend::~Backend() = default;

bool Backend::prepare() { return impl_->prepare(); }

std::optional<std::string> Backend::compile(std::string_view mir,
                                            const std::string &input_name,
                                            bool position_independent) {
  return impl_->compile(mir, input_name, position_independent);
}

} // namespace cephyr
