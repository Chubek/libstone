#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"

namespace moonstone::mahak {
inline namespace v1 {

enum class AdapterCapability : std::uint32_t {
    None = 0,
    Compile = 1u << 0,
    Link = 1u << 1,
    Execute = 1u << 2,
    PersistentProcess = 1u << 3,
    Startup = 1u << 4,
    Instrumentation = 1u << 5
};

inline AdapterCapability operator|(AdapterCapability a, AdapterCapability b) noexcept {
    return static_cast<AdapterCapability>(static_cast<std::uint32_t>(a) |
                                          static_cast<std::uint32_t>(b));
}
inline bool has_capability(AdapterCapability set, AdapterCapability wanted) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(wanted)) != 0;
}

struct AdapterInfo {
    std::string id;
    TargetKind kind = TargetKind::CustomCommand;
    AdapterCapability capabilities = AdapterCapability::None;
    std::string description;
};

struct ToolProbe {
    bool available = false;
    std::string name;
    std::string path;
    std::string version;
    std::string diagnostics;
};

// A target adapter turns a declarative target description into concrete stages
// and reports what it can actually do. An adapter must never claim a capability
// it does not implement; callers surface unsupported requests explicitly.
class Adapter {
  public:
    virtual ~Adapter() = default;
    virtual AdapterInfo info() const = 0;
    // Validates that the target is executable by this adapter and returns
    // normalized stages. The input target is not modified.
    virtual Result<std::vector<StageSpec>> prepare(const TargetSpec &target) const = 0;
};

// Generic manifest-driven adapter: every stage and command comes literally from
// the target description. This is the portable fallback.
class GenericCommandAdapter final : public Adapter {
  public:
    AdapterInfo info() const override;
    Result<std::vector<StageSpec>> prepare(const TargetSpec &target) const override;
};

// Adapter for C/C++ compiler drivers (cc, gcc, clang, clang++, c++). The target
// must provide a `compiler` parameter or a literal argv; fixtures are supplied
// by the benchmark. It generates compile/link stages only when the target
// declares the corresponding stage.
class NativeCompilerAdapter final : public Adapter {
  public:
    AdapterInfo info() const override;
    Result<std::vector<StageSpec>> prepare(const TargetSpec &target) const override;
};

// Adapter for already-built native executables.
class NativeExecutableAdapter final : public Adapter {
  public:
    AdapterInfo info() const override;
    Result<std::vector<StageSpec>> prepare(const TargetSpec &target) const override;
};

class AdapterRegistry {
  public:
    AdapterRegistry();
    void add(std::shared_ptr<const Adapter> adapter);
    const Adapter *find(std::string_view id) const;
    const Adapter *find_for(TargetKind kind) const;
    std::vector<AdapterInfo> list() const;

  private:
    std::vector<std::shared_ptr<const Adapter>> adapters_;
};

// Searches PATH without invoking a shell. Returns the absolute executable path
// or an explicit error. This is also used by `mahak doctor`.
Result<std::string> find_executable(std::string_view name);
// Runs `<path> --version` with a short timeout and captures the first line.
ToolProbe probe_tool(std::string_view name);

} // namespace v1
} // namespace moonstone::mahak
