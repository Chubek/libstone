#pragma once

#include "WASM-Module.hpp"

#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace limestone::wasmutils::core {

// Numeric values use their raw bits. v128 uses both halves, with bits holding
// the lowest lanes in binary order. high is zero for every other type, and
// the high 32 bits of i32/f32 are zero. A zero reference is null; non-null
// funcref bits encode a function index plus one in this instance. Externref
// bits are opaque to Wasm.
struct Value {
    Type type = Type::i32;
    std::uint64_t bits{};
    std::uint64_t high{};

    bool operator==(const Value&) const = default;
};

class Trap : public Error {
public:
    using Error::Error;
};

using HostCallback = std::function<std::vector<Value>(std::span<const Value>)>;

// Function imports are resolved by their exact (module, name) pair.
// The callback's argument span is valid only for that call. A callback can
// signal failure by throwing; its results are checked against the import type.
struct HostFunction {
    std::string module;
    std::string name;
    HostCallback callback;
};

namespace detail {
struct MemoryInstance;
struct TableInstance;
struct GlobalInstance;
}

// These copyable handles retain shared storage, not borrowed views. Instances
// and handles connected by shared imports must be externally serialized,
// including instance construction and destruction.
// Constructors and host accessors throw Error for invalid types or accesses.
class HostMemory {
public:
    explicit HostMemory(MemoryType type);
    HostMemory(const HostMemory&) noexcept = default;
    HostMemory& operator=(const HostMemory&) noexcept = default;

    // The minimum in type() is the current page count.
    [[nodiscard]] MemoryType type() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t count) const;
    void write(std::uint64_t offset, std::span<const std::uint8_t> bytes);

private:
    friend class Instance;
    std::shared_ptr<detail::MemoryInstance> storage_;
};

class HostTable {
public:
    explicit HostTable(TableType type);
    HostTable(const HostTable&) noexcept = default;
    HostTable& operator=(const HostTable&) noexcept = default;

    // Tables start filled with nulls. type() reports the current entry count
    // as its minimum.
    [[nodiscard]] TableType type() const noexcept;
    [[nodiscard]] Value get(std::uint64_t index) const;
    void set(std::uint64_t index, Value value);

private:
    friend class Instance;
    std::shared_ptr<detail::TableInstance> storage_;
};

class HostGlobal {
public:
    HostGlobal(GlobalType type, Value initial);
    HostGlobal(const HostGlobal&) noexcept = default;
    HostGlobal& operator=(const HostGlobal&) noexcept = default;

    [[nodiscard]] GlobalType type() const noexcept;
    [[nodiscard]] Value get() const noexcept;
    void set(Value value);

private:
    friend class Instance;
    std::shared_ptr<detail::GlobalInstance> storage_;
};

// Imported funcref tables and mutable funcref globals may be aliased within
// one instance, but cannot be bound to multiple live instances. Non-null
// function references are local indices: host handles do not accept them for
// direct initialization or writes. Use the owning instance's export API to
// write them or to interpret a non-null value read from a handle. The values
// are reset to null when that instance is destroyed (also on failed start);
// table sizes and other resources are not reset.
struct HostObjectImport {
    std::string module;
    std::string name;
    std::variant<HostMemory, HostTable, HostGlobal> object;
};

// Exact (module, name) keys must be unique across both spans. The spans are
// used only during construction; resources and callbacks are retained by value.
struct HostImports {
    std::span<const HostFunction> functions{};
    std::span<const HostObjectImport> objects{};
};

inline constexpr std::uint64_t default_fuel = 1'000'000;

struct InstanceOptions {
    std::uint64_t start_fuel = default_fuel;
    std::size_t max_call_depth = 1024; // Concurrent Wasm frames, excluding host calls.
    std::size_t max_locals = 65'536; // Parameters plus declared locals per function.
    std::size_t max_live_locals = 1'048'576; // Across all live frames in one invocation.
};

// An instance owns its module and retains its runtime state, sharing imported
// objects with the host. It is not thread-safe; invoking it again from one of
// its own host callbacks is not supported.
class Instance {
public:
    // Validate and instantiate the module, bind its imports, and run its start
    // function (if present). Invalid modules and missing imports throw Error;
    // start-function traps and out-of-bounds active initializers throw Trap.
    explicit Instance(Module module, std::span<const HostFunction> imports = {},
                      InstanceOptions options = {});

    // As above, with typed memory, table and global imports as well. Active
    // segments and the start function can mutate imported storage; a later
    // trap does not roll back these writes (except for expiring funcrefs).
    [[nodiscard]] static Instance with_imports(Module module, HostImports imports,
                                               InstanceOptions options = {});

    ~Instance();

    Instance(Instance&&) noexcept;
    Instance& operator=(Instance&&) noexcept;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // Each invocation has a new budget shared by all of its calls. One unit is
    // charged per dispatched instruction, or for a directly invoked import.
    // Wrong arguments or unknown functions throw Error; execution throws Trap
    // at the responsible instruction. Traps do not roll back instance state.
    std::vector<Value> invoke(std::uint32_t function, std::span<const Value> args = {},
                              std::uint64_t fuel = default_fuel);
    std::vector<Value> invoke(std::string_view name, std::span<const Value> args = {},
                              std::uint64_t fuel = default_fuel);

    // Access the instance's non-function exports by name. Memory is copied in
    // and out, so reads remain valid after memory.grow. Unknown or wrong-kind
    // exports, out-of-bounds accesses, and invalid writes throw Error, not Trap.
    [[nodiscard]] std::vector<std::uint8_t> read_memory(std::string_view name, std::uint64_t offset,
                                                        std::size_t count) const;
    void write_memory(std::string_view name, std::uint64_t offset, std::span<const std::uint8_t> bytes);
    [[nodiscard]] Value get_global(std::string_view name) const;
    void set_global(std::string_view name, Value value);
    [[nodiscard]] Value get_table(std::string_view name, std::uint64_t index) const;
    void set_table(std::string_view name, std::uint64_t index, Value value);

private:
    struct Impl;
    struct BindObjects {};
    Instance(Module module, HostImports imports, InstanceOptions options, BindObjects);
    Impl& export_instance() const;
    std::unique_ptr<Impl> impl_;
};

} // namespace limestone::wasmutils::core
