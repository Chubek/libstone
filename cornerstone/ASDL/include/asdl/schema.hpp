#ifndef ASDL_SCHEMA_HPP
#define ASDL_SCHEMA_HPP

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace asdl {

// Offsets count bytes from zero, lines and columns count from one. Spans
// are half-open and retain their filename after the input buffer is gone.
struct SourcePos {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct SourceSpan {
    std::string file;
    SourcePos begin;
    SourcePos end;
};

class SourceMap {
public:
    SourceMap(std::string_view file, std::string_view text);
    SourcePos position(std::size_t offset) const;
    SourceSpan span(std::size_t begin, std::size_t end) const;

private:
    std::string file_;
    std::size_t size_;
    std::vector<std::size_t> lines_;
};

class Diagnostic : public std::runtime_error {
public:
    Diagnostic(SourceSpan span, std::string message);
    const SourceSpan span;
    const std::string message;
};

using TypeId = std::size_t;
inline constexpr TypeId invalid_type_id = static_cast<TypeId>(-1);
using Options = std::map<std::string, std::string>;

struct Field {
    std::string type;
    std::string name;
    std::string c_name;
    char qualifier = 0;
    bool unnamed = false;
    SourceSpan span;
    TypeId type_id = invalid_type_id;
    std::string refinement;
    bool executable_refinement = false;
    SourceSpan refinement_span;
};

struct Constructor {
    std::string name;
    std::vector<Field> fields;
    std::size_t tag = 0;
    SourceSpan span;
};

enum class DefinitionKind { product, sum };

struct Definition {
    std::string name;
    DefinitionKind kind = DefinitionKind::product;
    std::vector<Field> fields;
    std::vector<Field> attributes;
    std::vector<Constructor> constructors;
    bool is_enum = false;
    SourceSpan span;
};

struct Generic {
    std::string kind;
    std::vector<std::string> args;
    SourceSpan span;
};

struct Fragment {
    std::string text;
    SourceSpan span;
};

struct ImportName {
    std::string name, alias;
    SourceSpan span;
};

struct Import {
    std::string module, alias;
    std::vector<ImportName> names;
    bool schema = false;
    SourceSpan span;
};

struct Logistics {
    std::string type;
    std::vector<std::string> parameters, synthesized;
    Fragment body;
    SourceSpan span;
};

struct ConstructorIndex {
    std::size_t definition;
    std::size_t constructor;
};

struct Schema {
    std::string file;
    std::string module; // empty means unwrapped definitions
    std::string prefix, upper, guard;
    std::vector<Definition> definitions;
    std::map<std::string, Generic> generics;
    Options options;
    std::optional<Fragment> prologue, epilogue;
    std::set<std::string> used_builtins, omitted_builtins;

    // Rebuilt by validate(). Indices, rather than pointers, preserve value
    // semantics. Do not use indices after modifying the schema's declarations.
    std::map<std::string, std::size_t> types;
    std::map<std::string, ConstructorIndex> constructors;
    std::map<std::string, TypeId> type_ids;
    std::vector<std::string> type_names;

    std::string language, summary, version, unit;
    SourceSpan unit_span;
    bool modular = false;
    bool resolved = false;
    std::vector<Import> imports;
    std::vector<Logistics> logistics;
};

Schema parse(std::string_view text, std::string_view file = "<input>");
Schema parse_file(const std::filesystem::path& path,
                  const std::vector<std::filesystem::path>& import_roots = {});
void resolve(Schema& schema, const std::vector<std::filesystem::path>& import_roots = {});
// Apply defaults, then source directives, then explicit overrides.
void resolve_options(Schema& schema, const Options& overrides = {});
// Normalize names, check definitions and dependencies, and build indices.
void validate(Schema& schema);

std::string type_stem(std::string_view type);
std::string c_type(const Schema& schema, std::string_view type);
std::string field_type(const Schema& schema, const Field& field);
std::string tag_name(const Schema& schema, const Definition& definition,
                     const Constructor& constructor);
const std::map<std::string, std::string>& builtin_c_types();
const std::map<std::string, std::string>& scalar_c_types();
const std::set<std::string>& primitives();
std::vector<std::string> type_dependencies(const Schema& schema, std::string_view type);
std::vector<std::string> primitive_types(const Schema& schema);
std::vector<std::string> all_types(const Schema& schema);

} // namespace asdl

#endif
