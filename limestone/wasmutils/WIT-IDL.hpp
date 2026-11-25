#pragma once

#include <cstddef>
#include <optional>
#include <span>
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
    // Zero-based index into the binding resolver's dependency span, if the error is there.
    std::optional<std::size_t> dependency_index{};

    WitError(WitLocation at, const std::string& message)
        : std::runtime_error(message), location(at) {}
};

// Kebab-case WIT spelling, without an optional leading '%'.
struct WitName {
    std::string text;
    WitLocation location;
};

struct WitVersion {
    std::string text; // SemVer, without '@'.
    WitLocation location;
};

struct WitDocComment {
    std::string text; // Comment text without the opening or closing delimiters.
    WitLocation location;
};

struct WitAttributeArgument {
    WitName name;
    std::string value; // Source spelling, including quotes for strings.
    WitLocation location; // Start of the value.
};

struct WitAttribute {
    WitName name;
    std::vector<WitAttributeArgument> arguments;
};

struct WitAnnotations {
    std::vector<WitDocComment> docs;
    std::vector<WitAttribute> attributes;
};

struct WitPackage {
    WitName name_space;
    WitName name;
    std::optional<WitVersion> version;
    WitAnnotations annotations{};
};

// Unqualified `name`, or `namespace:package/name` with an optional `@version`.
struct WitPath {
    std::optional<WitPackage> package;
    WitName name;
};

struct WitStandaloneUse {
    WitPath path;
    std::optional<WitName> alias;
    WitAnnotations annotations{};
};

struct WitUseItem {
    WitName name;
    std::optional<WitName> alias;
};

struct WitUse {
    WitPath path;
    std::vector<WitUseItem> items;
    WitAnnotations annotations{};
};

enum class WitScalar { Bool, U8, S8, U16, S16, U32, S32, U64, S64, F32, F64, Char };
enum class WitTypeKind { Scalar, String, Named, List, Option, Tuple, Result, Own, Borrow, Unit };

struct WitType {
    WitScalar scalar = WitScalar::Bool;
    WitLocation location;
    WitTypeKind kind = WitTypeKind::Scalar;
    std::optional<WitName> name{}; // Named, Own or Borrow.
    // List/Option: one argument. Tuple: elements. Result: zero to two
    // arguments; Unit is the first argument only for result<_, E>.
    std::vector<WitType> arguments{};
};

struct WitParameter {
    WitName name;
    WitType type;
    WitAnnotations annotations{}; // Also used for record fields.
};

enum class WitFunctionKind { Freestanding, Constructor, Method, Static };

struct WitFunction {
    WitName name;
    std::vector<WitParameter> parameters;
    std::optional<WitType> result; // Constructors have an implicit owned result instead.
    WitFunctionKind kind = WitFunctionKind::Freestanding;
    WitAnnotations annotations{};
};

enum class WitTypeDefKind { Alias, Record, Variant, Enum, Flags, Resource };

struct WitCase {
    WitName name;
    std::optional<WitType> payload; // Variant only.
    WitAnnotations annotations{};
};

struct WitTypeDef {
    WitName name;
    WitTypeDefKind kind = WitTypeDefKind::Alias;
    std::optional<WitType> alias{}; // Alias target.
    std::vector<WitParameter> fields{}; // Record fields.
    std::vector<WitCase> cases{}; // Variant, enum or flags cases.
    std::vector<WitFunction> functions{}; // Resource members.
    WitAnnotations annotations{};
};

struct WitInterface {
    WitName name;
    std::vector<WitFunction> functions;
    std::vector<WitTypeDef> types{};
    std::vector<WitUse> uses{};
    WitAnnotations annotations{};
};

enum class WitWorldItemKind { Path, Function, Interface };

struct WitWorldItem {
    WitWorldItemKind kind = WitWorldItemKind::Path;
    bool is_import = false;
    // Exactly one payload is present. Inline functions/interfaces own their
    // names; path-based items use their full path as the item key.
    std::optional<WitPath> path{};
    std::optional<WitFunction> function{};
    std::optional<WitInterface> interface{};
    WitAnnotations annotations{};
};

struct WitInclude {
    WitPath path;
    std::vector<WitUseItem> renames; // Every rename has an alias.
    WitAnnotations annotations{};
};

struct WitWorld {
    WitName name;
    std::vector<WitWorldItem> items{};
    std::vector<WitTypeDef> types{};
    std::vector<WitUse> uses{};
    std::vector<WitInclude> includes{};
    WitAnnotations annotations{};
};

struct WitDocument {
    std::optional<WitPackage> package;
    std::vector<WitInterface> interfaces;
    std::vector<WitWorld> worlds{};
    std::vector<WitStandaloneUse> uses{};
};

// Emits native host prototypes (the implementation is supplied separately),
// not component imports/exports, canonical ABI adapters, or a wire format.
// See WIT-Canonical.hpp for the separate synchronous Canonical ABI API.
// C linkage names begin with wit_. ASCII case is folded and '-' becomes '_'.
// Unpackaged names use wit_<interface>_W_<function>; packaged ones prepend
// <namespace>_W_<package>_W_ and, if versioned, v<major>v<minor>v<patch>_W_.
// SemVer prerelease/build suffixes, if present, are hex-escaped in the version.
// Parameters use wit_arg_<name>. The C compiler/linker must distinguish
// the full names, not just the minimum significant prefixes of C90.
// The header describes the host types and their value conventions.
// Throws WitError on an invalid (including manually constructed) AST.
std::string emit_wit_c(const WitDocument& document);

// Resolve references in the input and its reachable dependencies using only
// the supplied documents. Each dependency must declare a unique package;
// names and versions match exactly (including whether a version is present).
// Dependencies supply referenced declarations, not unrelated entry points.
std::string emit_wit_c(const WitDocument& document, std::span<const WitDocument> dependencies);

// Convenience entry point: parse_wit followed by emit_wit_c.
std::string wit_to_c(std::string_view source);

} // namespace limestone::wasmutils
