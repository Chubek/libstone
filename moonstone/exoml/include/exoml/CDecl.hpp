#pragma once
#if __has_include("../../../core/Core.hpp")
#include "../../../core/Core.hpp"
#else
#include <moonstone/core/Core.hpp>
#endif

namespace moonstone::exoml::cdecl {
inline constexpr unsigned schema_version = 2;
inline constexpr std::string_view extractor_version = "exoml-c11-3";

enum class TypeKind {
    Unknown,
    Void,
    Integer,
    Floating,
    Pointer,
    Array,
    Function,
    Typedef,
    Record,
    Enum,
    Elaborated
};
enum class DeclKind { Unsupported, Function, Typedef, Record, Enum, Variable, Macro };
struct Qualifiers {
    bool is_const = false, is_volatile = false, is_restrict = false;
};
struct Field {
    std::string name, type;
    SourceSpan source;
    std::optional<std::uint64_t> offset_bits;
    std::optional<unsigned> bit_width;
};
struct Enumerator {
    std::string name, value;
};
struct Type {
    // IDs are compiler/target/argument-scoped; transport with the unit fingerprint
    // when comparing different source revisions, not as unqualified global IDs.
    std::string id, canonical_id, spelling, canonical_spelling, declaration_id;
    TypeKind kind = TypeKind::Unknown;
    Qualifiers qualifiers;
    std::optional<std::uint64_t> size, alignment, array_count;
    std::optional<bool> is_signed;
    // Pointee, array element, typedef/elaborated target or enum underlying integer type.
    std::string element, result;
    std::vector<std::string> arguments;
    std::vector<Field> fields;
    std::vector<Enumerator> enumerators;
    bool variadic = false, prototype = false, is_union = false;
    // Numeric CXCallingConv, scoped to recorded compiler identity. Not a portable enum.
    unsigned calling_convention = 0;
    std::vector<std::string> attributes;
    std::string unsupported_reason;
};
struct Parameter {
    std::string name, type;
    SourceSpan source;
    std::string tokens;
};
struct Declaration {
    std::string id, name, type;
    DeclKind kind = DeclKind::Unsupported;
    SourceSpan source;
    std::vector<Parameter> parameters;
    bool definition = false, is_inline = false, function_like_macro = false;
    std::string linkage, tokens, unsupported_reason;
};
struct InputFile {
    std::string path, sha256;
};
struct Target {
    std::string compiler, triple;
    unsigned pointer_bits = 0;
};
struct CompilationContext {
    std::string database, directory, file, driver;
    std::vector<std::string> original_arguments, ignored_arguments;
};
struct MacroDefinition {
    std::string id, name;
    std::vector<std::string> tokens;
    SourceSpan source;
    bool function_like = false, builtin = false;
};
struct MacroExpansion {
    std::string name, definition_id, unknown_reason;
    SourceSpan source;
};
struct MacroState {
    std::string name, definition_id, unknown_reason;
    std::optional<bool> defined;
};
struct IncludeEdge {
    std::string included;
    SourceSpan source;
};
struct Preprocessing {
    std::vector<MacroDefinition> definitions;
    std::vector<MacroExpansion> expansions;
    std::vector<MacroState> final_macros;
    std::vector<IncludeEdge> includes;
    std::vector<SourceSpan> skipped_ranges;
    bool final_snapshot_complete = false;
    std::string coverage =
        "libclang source events; final defined states for reported/CLI names and standard dynamic "
        "builtins; nested replacement trees are not exposed";
};
struct TranslationUnit {
    unsigned version = schema_version;
    std::string header, revision, fingerprint;
    Target target;
    std::vector<std::string> arguments;
    std::vector<InputFile> inputs;
    std::map<std::string, Type> types;
    std::vector<Declaration> declarations;
    Diagnostics diagnostics;
    bool complete = false;
    std::optional<CompilationContext> compilation;
    Preprocessing preprocessing;
};
// Deterministic discovery JSON, not v1 BindingModule reflection or a safe contract.
std::string describe_translation_unit(const TranslationUnit &unit);
// Parse the JSON output of describe_translation_unit back into a TranslationUnit.
// Extracts declaration names, kinds and parameters sufficient for contract validation;
// type details and preprocessing evidence are preserved when present.
TranslationUnit parse_translation_unit_json(std::string_view json);
} // namespace moonstone::exoml::cdecl
