#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace limestone::wasmutils {

// Zero-based byte offset and one-based line and byte column.
struct WitLocation {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct WitError : std::runtime_error {
    WitLocation location;

    WitError(WitLocation at, const std::string& message)
        : std::runtime_error(message), location(at) {}
};

// Kebab-case WIT spelling, without an optional leading '%'.
struct WitName {
    std::string text;
    WitLocation location;
};

struct WitVersion {
    std::string text; // major.minor.patch, without '@'
    WitLocation location;
};

struct WitPackage {
    WitName name_space;
    WitName name;
    std::optional<WitVersion> version;
};

enum class WitScalar { Bool, U8, S8, U16, S16, U32, S32, U64, S64, F32, F64, Char };

struct WitType {
    WitScalar scalar = WitScalar::Bool;
    WitLocation location;
};

struct WitParameter {
    WitName name;
    WitType type;
};

struct WitFunction {
    WitName name;
    std::vector<WitParameter> parameters;
    std::optional<WitType> result;
};

struct WitInterface {
    WitName name;
    std::vector<WitFunction> functions;
};

struct WitDocument {
    std::optional<WitPackage> package;
    std::vector<WitInterface> interfaces;
};

// Emits native host prototypes (the implementation is supplied separately),
// not component imports/exports, canonical ABI adapters, or a wire format.
// C linkage names begin with wit_. ASCII case is folded and '-' becomes '_'.
// Unpackaged names use wit_<interface>_W_<function>; packaged ones prepend
// <namespace>_W_<package>_W_ and, if versioned, v<major>v<minor>v<patch>_W_.
// Parameters use wit_arg_<name>. The C compiler/linker must distinguish
// the full names, not just the minimum significant prefixes of C90.
// The header describes the host types and their value conventions.
// Throws WitError on an invalid (including manually constructed) AST.
std::string emit_wit_c(const WitDocument& document);

// Convenience entry point: parse_wit followed by emit_wit_c.
std::string wit_to_c(std::string_view source);

} // namespace limestone::wasmutils
