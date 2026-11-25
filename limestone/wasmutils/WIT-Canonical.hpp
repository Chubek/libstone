#pragma once

#include "WASM-Instance.hpp"
#include "WIT-IDL.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <variant>

namespace limestone::wasmutils::canonical {

// Synchronous memory32 Canonical ABI with UTF-8 strings. This is a value and
// call adapter, not a component loader or linker. It does not model component
// instance state, resource handles, async operations, or other canon options.
// Layout follows the Component Model CanonicalABI.md (585526aa); empty
// aggregates and flags outside 1..32 labels are not component value types.
inline constexpr std::size_t max_flat_params = 16;
inline constexpr std::size_t max_flat_results = 1;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class Kind { Scalar, String, List, Tuple, Record, Option, Result, Variant, Enum, Flags };
struct Field;

// An immutable, shared layout. A default-constructed Type means absent
// (a void result, no parameters, or a variant arm without a payload).
class Type {
public:
    Type() = default;
    explicit operator bool() const noexcept { return data_ != nullptr; }
    Kind kind() const;
    WitScalar scalar() const;
    std::uint32_t size() const;
    std::uint32_t alignment() const;
    // Sequence fields, cases, or labels in WIT order; one field for a list's
    // element type. Returned views are valid as long as a copy of Type lives.
    std::span<const Field> fields() const;
    std::span<const std::uint32_t> field_offsets() const; // Record/tuple only.
    std::uint32_t discriminant_size() const; // Option/result/variant/enum only.
    std::uint32_t payload_offset() const;
    std::size_t flat_count() const;
    // Complete when flat_count() <= max_flat_params, otherwise a prefix.
    std::span<const core::Type> flat_prefix() const;

    static Type scalar(WitScalar value);
    static Type string();
    static Type list(Type element);
    static Type sequence(Kind kind, std::vector<Field> fields);
    static Type choice(Kind kind, std::vector<Field> cases);
    static Type flags(std::vector<Field> labels);

private:
    struct Data;
    explicit Type(std::shared_ptr<const Data> data);
    const Data& get() const;
    std::shared_ptr<const Data> data_;
};

struct Field {
    std::string name;
    Type type; // Absent for flags labels or cases without a payload.
};

struct Value {
    using Sequence = std::vector<Value>; // List, record, or tuple in order.
    struct Case {
        std::uint32_t tag{};     // Zero-based index in declaration order.
        Sequence payload;        // Zero or one value, depending on the tag.
        bool operator==(const Case&) const = default;
    };
    struct Flags {
        std::uint32_t bits{};    // Bit 0 is the first label.
        bool operator==(const Flags&) const = default;
    };
    using Data = std::variant<bool, std::uint64_t, std::int64_t, float, double,
                              char32_t, std::string, Sequence, Case, Flags>;
    Data data; // Strings are UTF-8, unsigned/signed integers use separate alternatives.
    bool operator==(const Value&) const = default;
};

enum class Direction { Lift, Lower };

struct Function {
    std::string name; // Stable diagnostic ID, using native wit2c's spelling; not a core symbol.
    Type parameters;  // Tuple of named parameters, or absent.
    Type result;      // A single component value, or absent.
    // Standalone interfaces are neutral; world imports are Lower, exports Lift.
    std::optional<Direction> world_direction;
};

struct Signature {
    std::vector<core::Type> parameters;
    std::vector<core::Type> results;
    bool indirect_parameters{};
    bool indirect_result{};
};

Signature signature(const Function& function, Direction direction);

// Resolve the same interfaces, world items/includes, and explicitly supplied
// dependencies as emit_wit_c. Unsupported WIT features throw WitError at
// their declaration or use; irrelevant functions in dependencies aren't emitted.
std::vector<Function> resolve(const WitDocument& document, std::span<const WitDocument> dependencies = {});
std::string describe(const WitDocument& document, std::span<const WitDocument> dependencies = {});

struct Limits {
    // Independent of the ABI's fixed byte-size limit. These per-call/operation
    // budgets bound host amplification through repeated/overlapping guest lists.
    std::size_t values = 1'000'000;
    std::size_t string_bytes = 64 * 1024 * 1024;
};

struct Memory {
    // A fresh view is requested after calls to realloc/callees; those callbacks
    // may grow (but not shrink) memory. The view must remain valid until then.
    // The memory or allocator may be absent when the call doesn't need it.
    std::function<std::span<std::uint8_t>()> bytes;
    // Canonical realloc(old_pointer, old_size, alignment, new_size). The
    // adapter passes 0 for the first two values; allocations must not overlap
    // any live block. The embedding owns allocation lifetime and trap policy.
    std::function<std::uint32_t(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)> realloc;
    Limits limits{};
};

// These APIs check guest bounds, alignment, types and representation; invalid
// data throws Error. Embedding exceptions are propagated. Stores/allocations
// already performed are not rolled back.
Value load(const Type& type, const Memory& memory, std::uint32_t pointer);
void store(const Type& type, const Value& value, const Memory& memory, std::uint32_t pointer);

using ComponentCall = std::function<std::optional<Value>(std::span<const Value>)>;
using CoreCall = std::function<std::vector<core::Value>(std::span<const core::Value>)>;
using PostReturn = std::function<void(std::span<const core::Value>)>;

// Lifts a core callee into a component-level call. An indirect result is an
// i32 returned by callee. post_return receives the raw core results once the
// result has been copied out successfully.
std::optional<Value> call_lifted(const Function& function, std::span<const Value> arguments,
                                 const Memory& memory, const CoreCall& callee,
                                 const PostReturn& post_return = {});
// Lowers a component callee into a core call. An indirect result goes to the
// caller's memory at the out-pointer in the last core argument; no core value
// is returned in that case. Callbacks must not retain argument spans.
std::vector<core::Value> call_lowered(const Function& function,
                                     std::span<const core::Value> arguments,
                                     const Memory& memory, const ComponentCall& callee);

} // namespace limestone::wasmutils::canonical
