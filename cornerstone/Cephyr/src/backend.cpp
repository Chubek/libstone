#include "cephyr/Cephyr.h"
#include "cephyr/backend.hpp"
#include "../FactIR.hpp"

#include "cephyr/config.hpp"
#include "cephyr/diagnostics.hpp"
#include "cephyr/host_target.hpp"
#include "cephyr/target.hpp"

#include <Backend.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

unsigned cephyr_python_version(void) {
  return 0; // No interpreter ABI is linked into this library.
}

namespace cephyr {
namespace {

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

} // namespace

struct Backend::Impl {
  using CompileText = ArchonBackendStatus (*)(const char *, std::size_t,
                                              const ArchonBackendOptions *,
                                              unsigned, ArchonBackendArtifact *,
                                              ArchonBackendDiagnostic *);
  using CompileWithHints = decltype(&archon_backend_compile_text_with_schedule_hints);
  using CompileWithFacts = decltype(&archon_backend_compile_text_with_facts);
  using DestroyArtifact = void (*)(ArchonBackendArtifact *);
  using BackendTarget = const char *(*)();

  const Config &config;
  const Target &target;
  Diagnostics &diagnostics;

  bool attempted = false;
  bool ready = false;
  void *library = nullptr;
  CompileText compile_text = nullptr;
  CompileWithHints compile_with_hints = nullptr;
  CompileWithFacts compile_with_facts = nullptr;
  DestroyArtifact destroy_artifact = nullptr;
  BackendTarget backend_target = nullptr;

  Impl(const Config &config, const Target &target,
       Diagnostics &diagnostics)
      : config(config), target(target),
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
    const std::string expected = target.architecture + "/" + target.abi;
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
    // Optional additive Archon API; older configured packages still work.
    compile_with_hints = reinterpret_cast<CompileWithHints>(
        dlsym(library, "archon_backend_compile_text_with_schedule_hints"));
    compile_with_facts = reinterpret_cast<CompileWithFacts>(
        dlsym(library, "archon_backend_compile_text_with_facts"));
    return true;
  }

  bool prepare_impl() {
    const auto* provider = native_lowering_provider(target.architecture, target.abi);
    if (!provider) {
      error("cephyr.backend.target", "unsupported native backend target");
      return false;
    }
    const auto directory = config.package_root / target.architecture;
    if (config.representation != "gated" ||
        !std::filesystem::equivalent(target.specification, config.archon_root / provider->stdspec)) {
      error("cephyr.backend.target", "no packaged backend matches the selected specification and representation");
      return false;
    }
    // Verify the complete imported source closure using build-generated digests.
    // No architecture frontend, interpreter or native code generation at runtime.
    std::ifstream manifest(directory / "sources.sha256");
    std::string line;
    bool saw_root = false;
    while (std::getline(manifest, line)) {
      if (line.size() < 66 || line[64] != ' ' ||
          line.find_first_not_of("0123456789abcdef") != 64) {
        error("cephyr.backend.resources", "invalid source digest in " + directory.string());
        return false;
      }
      const std::filesystem::path relative = line.substr(65);
      std::string bytes, failure;
      if (!read_file(config.archon_root / relative, bytes, failure)) {
        error("cephyr.backend.resources", failure); return false;
      }
      Sha256 hash;
      hash.update(bytes);
      if (hash.finish() != line.substr(0, 64)) {
        error("cephyr.backend.target", "architecture source differs from packaged backend: " + relative.string());
        return false;
      }
      saw_root |= relative == provider->stdspec;
    }
    if (!manifest.eof() || !saw_root) {
      error("cephyr.backend.resources", "invalid or missing packaged source manifest: " + directory.string());
      return false;
    }
    if (config.verbose)
      diagnostics.note("cephyr.backend", "loading packaged Archon backend " + directory.string());
    return load_library(directory / "libbackend.so");
  }

  bool prepare() {
    if (attempted)
      return ready;
    attempted = true;
    try {
      ready = prepare_impl();
    } catch (const std::filesystem::filesystem_error &failure) {
      error("cephyr.backend.io", failure.what());
    } catch (const std::exception &failure) {
      error("cephyr.backend.internal", failure.what());
    }
    return ready;
  }

  std::optional<std::string> compile(std::string_view mir,
                                     const std::string &input_name,
                                     bool position_independent, const factir::Result *facts) {
    if (!prepare())
      return std::nullopt;
    ArchonBackendArtifact artifact{};
    ArchonBackendDiagnostic diagnostic{};
    ArchonBackendOptions options{};
    const unsigned flags =
        position_independent ? static_cast<unsigned>(ARCHON_BACKEND_PIC) : 0u;
    if (facts && !facts->matches(mir, target))
      facts = nullptr;
    const bool rv64 = target.architecture == "RISCV64IM" || target.architecture == "RISCV64I";
    const bool use_schedule = facts && !facts->preferred.empty() && rv64;
    const bool use_facts = facts && compile_with_facts &&
        (!facts->coordinates.empty() || !facts->known_bits.empty() || !facts->preferred.empty());
    struct HintInput {
      const factir::Result *facts;
      std::size_t accepted = 0;
    } hint_input{facts};
    const ArchonBackendScheduleHints hints{
        [](const void *user, std::size_t function, std::size_t block, ArchonValue dst) noexcept -> int {
          auto &input = *static_cast<HintInput *>(const_cast<void *>(user));
          const int preferred = factir::Result::prefer(input.facts, function, block, dst);
          input.accepted += preferred;
          return preferred;
        }, &hint_input};
    const ArchonBackendFactHints native_facts{
        [](const void *user, ArchonBackendFactKind kind, std::size_t function,
           std::size_t block, std::uint32_t key) noexcept -> int {
          auto &input = *static_cast<HintInput *>(const_cast<void *>(user));
          const bool found = input.facts->contains(kind, function, block, key);
          if (kind == ARCHON_BACKEND_FACT_OBSERVABLE_INPUT)
            input.accepted += found;
          return found;
        },
        [](const void *user, std::size_t function, ArchonValue value,
           std::uint64_t *zero, std::uint64_t *one) noexcept -> int {
          const auto &input = *static_cast<const HintInput *>(user);
          return factir::Result::query_known_bits(input.facts, function, value, zero, one);
        }, &hint_input};
    const auto status = use_facts ?
        compile_with_facts(mir.data(), mir.size(), &options,
                           flags | (use_schedule ? static_cast<unsigned>(ARCHON_BACKEND_SCHEDULE_PURE) : 0u),
                           &native_facts, &artifact, &diagnostic) :
        use_schedule && compile_with_hints ?
        compile_with_hints(mir.data(), mir.size(), &options, flags | ARCHON_BACKEND_SCHEDULE_PURE,
                           &hints, &artifact, &diagnostic) :
        compile_text(mir.data(), mir.size(), &options, flags, &artifact, &diagnostic);
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
    if (use_schedule && config.verbose) {
      if (use_facts)
        diagnostics.note("cephyr.factir.schedule", "Archon FactIR scheduling/allocation received " +
            std::to_string(hint_input.accepted) + " positive observable-value answer(s)", Location{input_name});
      else if (compile_with_hints)
        diagnostics.note("cephyr.factir.schedule", "Archon pure scheduler preferred " +
            std::to_string(hint_input.accepted) + " FactIR value(s)", Location{input_name});
    }
    return std::string(artifact.data, artifact.size);
  }
};

Backend::Backend(const Config &config, const Target &target,
                 Diagnostics &diagnostics)
    : impl_(std::make_unique<Impl>(config, target, diagnostics)) {}

Backend::~Backend() = default;

bool Backend::prepare() { return impl_->prepare(); }

std::optional<std::string> Backend::compile(std::string_view mir,
                                             const std::string &input_name,
                                             bool position_independent,
                                             const factir::Result *facts) {
  return impl_->compile(mir, input_name, position_independent, facts);
}

} // namespace cephyr
