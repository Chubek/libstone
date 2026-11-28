#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Model.hpp"

// Native plugin ABI for Mahak's SDK.
//
// A plugin is a shared object linked against -lmahaksdk that exports exactly
// one symbol:
//
//     int mahak_plugin_init_v1(const PluginHostApi &, PluginRegistration &) noexcept;
//
// The host passes its own size-tagged table and an empty registration record.
// The plugin fills the registration with a descriptor table and a parallel hook
// table, then returns 0. Any other return value, an unexpected abi_version, an
// undersized struct, a duplicate id, or a missing hook for the declared kind
// makes the load fail and every partially registered entry is torn down.
//
// Ownership: the plugin owns descriptors and hooks for as long as it is
// loaded. Memory handed back to the host through an output parameter belongs to
// the host. The host calls teardown once, after no callback is running, and
// only then unloads the library. Callbacks must not throw; a throw is a harness
// failure, not a plugin verdict.
//
// A plugin declares only what it implements. The host never invents a
// capability: a registered oracle that returns a defaulted OracleResult is a
// contract violation, not a pass.

namespace moonstone::mahak {
inline namespace v1 {

inline constexpr std::uint32_t mahak_plugin_abi_version = 1;
inline constexpr const char *mahak_plugin_entry_symbol = "mahak_plugin_init_v1";

// A borrowed string. Not NUL-terminated; the producer guarantees the bytes stay
// valid for the lifetime of the call or of the loaded plugin (descriptors).
struct StringView {
    const char *data = nullptr;
    std::size_t size = 0;

    std::string_view value() const noexcept {
        return data != nullptr ? std::string_view(data, size) : std::string_view();
    }
};

inline StringView make_string_view(std::string_view text) noexcept {
    return StringView{text.data(), text.size()};
}

enum class PluginKind : std::uint32_t {
    FixtureGenerator,
    Oracle,
    Measurement,
    Analysis,
    Predictor,
    Reporter
};

const char *name(PluginKind kind) noexcept;
std::optional<PluginKind> parse_plugin_kind(std::string_view text) noexcept;

// Qualified ids are "<vendor>.<component>[.<detail>] using lower-case ASCII,
// digits, '_' and '.'; they are the only stable handles the host stores.
bool valid_plugin_id(std::string_view id) noexcept;

enum class PluginCapability : std::uint32_t {
    None = 0,
    FixtureBytes = 1u << 0,
    Judge = 1u << 1,
    Measure = 1u << 2,
    Analyze = 1u << 3,
    Predict = 1u << 4,
    Report = 1u << 5,
    Deterministic = 1u << 6
};

inline PluginCapability operator|(PluginCapability a, PluginCapability b) noexcept {
    return static_cast<PluginCapability>(static_cast<std::uint32_t>(a) |
                                         static_cast<std::uint32_t>(b));
}
inline bool has_capability(PluginCapability set, PluginCapability wanted) noexcept {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(wanted)) != 0;
}

// --------------------------- host -> plugin data ---------------------------

struct PluginHostApi {
    std::uint32_t struct_size = sizeof(PluginHostApi);
    std::uint32_t abi_version = mahak_plugin_abi_version;
    void *host_data = nullptr;
    // Paired allocator. Any buffer the plugin hands to the host through
    // allocate/deallocate must use this pair.
    void *(*allocate)(void *host_data, std::size_t size) noexcept = nullptr;
    void (*deallocate)(void *host_data, void *pointer) noexcept = nullptr;
    // Advisory log sink; the message is not retained after the call.
    void (*log)(void *host_data, int severity, StringView message) noexcept = nullptr;
};

// ------------------------------- plugin table ------------------------------

struct PluginDescriptor {
    std::uint32_t struct_size = sizeof(PluginDescriptor);
    std::uint32_t abi_version = mahak_plugin_abi_version;
    StringView id;
    StringView version;
    StringView description;
    PluginKind kind = PluginKind::Oracle;
    PluginCapability capabilities = PluginCapability::None;
    void *user_data = nullptr;
};

// One entry per descriptor, same order. Only the hook for the declared kind is
// required; the others may be null. `judge` returns the verdict the host would
// otherwise compute, so it must set validity truthfully.
struct PluginHooks {
    std::uint32_t struct_size = sizeof(PluginHooks);
    OracleResult (*judge)(void *user_data, const CasePlan &plan,
                          const CommandResult &command) = nullptr;
    Observation (*measure)(void *user_data, const CasePlan &plan,
                           const CommandResult &command) = nullptr;
    Json (*analyze)(void *user_data, const SessionRecord &session) = nullptr;
    Json (*predict)(void *user_data, const std::vector<Observation> &observations) = nullptr;
    // Appends the rendered report to `out`; the host owns the string.
    bool (*report)(void *user_data, const SessionRecord &session, std::string &out) = nullptr;
    // Writes generated fixture bytes to `out`; deterministic for one seed.
    bool (*generate)(void *user_data, std::uint64_t seed, std::string &out) = nullptr;
};

struct PluginRegistration {
    std::uint32_t struct_size = sizeof(PluginRegistration);
    const PluginDescriptor *descriptors = nullptr;
    const PluginHooks *hooks = nullptr;
    std::size_t count = 0;
    // Called once, after the last callback returned. May be null.
    void (*teardown)(void *user_data) noexcept = nullptr;
    void *user_data = nullptr;
};

using PluginInit = int (*)(const PluginHostApi &host, PluginRegistration &registration) noexcept;

// ------------------------------ host side ----------------------------------

struct PluginInfo {
    std::string id;
    std::string version;
    std::string description;
    PluginKind kind = PluginKind::Oracle;
    PluginCapability capabilities = PluginCapability::None;
    std::string library; // path the plugin was loaded from
};

struct PluginLoadOptions {
    // A loaded plugin's callbacks run inside the harness process. Only load
    // libraries you already trust to run with the caller's privileges.
    std::size_t max_plugins = 32;
    bool verbose = false;
};

class PluginRegistry {
  public:
    static Result<PluginRegistry> create(PluginHostApi host = {});
    ~PluginRegistry();
    PluginRegistry(PluginRegistry &&) noexcept;
    PluginRegistry &operator=(PluginRegistry &&) noexcept;
    PluginRegistry(const PluginRegistry &) = delete;
    PluginRegistry &operator=(const PluginRegistry &) = delete;

    // Loads one library, validates its registration, and rejects duplicates.
    // On failure the library is unloaded and nothing remains registered.
    Result<void> load(std::string_view path, const PluginLoadOptions &options = {});

    std::vector<PluginInfo> list() const;
    // Borrowed pointers; callers must externally synchronize their use
    // (including raw hook calls) with teardown, moves and destruction.
    const PluginHooks *hooks(std::string_view id) const;
    const PluginInfo *info(std::string_view id) const;

    // Dispatches an oracle with its registered user data. Teardown waits for
    // in-flight dispatch and rejects new dispatch while teardown is pending.
    Result<OracleResult> judge(std::string_view id, const CasePlan &plan,
                               const CommandResult &command) const;

    // Calls every teardown, then unloads. Idempotent; also runs on destruction.
    void teardown();

  private:
    PluginRegistry();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace v1
} // namespace moonstone::mahak

extern "C" std::uint32_t mahak_sdk_version() noexcept;
extern "C" void mahak_sdk_fill_host_api(moonstone::mahak::PluginHostApi *host) noexcept;
