#include "asdl/schema.hpp"

#include <algorithm>
#include <utility>

namespace asdl {
namespace {

bool letter(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool digit(char c)
{
    return c >= '0' && c <= '9';
}

bool identifier(std::string_view name)
{
    return !name.empty() && letter(name.front()) &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return letter(c) || digit(c) || c == '_';
        });
}

std::string upper(std::string text)
{
    for (char& c : text)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return text;
}

std::string lower(std::string text)
{
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string diagnostic_text(const SourceSpan& span, const std::string& message)
{
    return span.file + ':' + std::to_string(span.begin.line) + ':' +
           std::to_string(span.begin.column) + ": " + message;
}

const std::set<std::string>& c_reserved()
{
    static const auto names = [] {
        std::set<std::string> values{
            "auto", "break", "case", "char", "const", "continue", "default",
            "do", "double", "else", "enum", "extern", "float", "for", "goto",
            "if", "inline", "int", "long", "register", "restrict", "return",
            "short", "signed", "sizeof", "static", "struct", "switch",
            "typedef", "union", "unsigned", "void", "volatile", "while",
            "_Bool", "_Complex", "_Imaginary", "_Alignas", "_Alignof",
            "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local",
            "NULL", "EXIT_SUCCESS", "EXIT_FAILURE", "RAND_MAX", "MB_CUR_MAX",
            "SIZE_MAX", "PTRDIFF_MIN", "PTRDIFF_MAX", "SIG_ATOMIC_MIN",
            "SIG_ATOMIC_MAX", "WCHAR_MIN", "WCHAR_MAX", "WINT_MIN", "WINT_MAX",
            "bool", "true", "false", "alignas", "alignof", "EOF", "stdin",
            "stdout", "stderr", "errno",
        };
        for (const std::string_view width : {"8", "16", "32", "64",
                "LEAST8", "LEAST16", "LEAST32", "LEAST64", "FAST8", "FAST16",
                "FAST32", "FAST64", "PTR", "MAX"}) {
            values.insert("INT" + std::string(width) + "_MIN");
            values.insert("INT" + std::string(width) + "_MAX");
            values.insert("UINT" + std::string(width) + "_MAX");
        }
        return values;
    }();
    return names;
}

void name_fields(std::vector<Field>& fields, const std::set<std::string>& reserved)
{
    auto used = reserved;
    std::set<std::string> originals;
    for (auto& field : fields) {
        // Reset previous synthesized names so revalidating is idempotent
        // and edits to the field list can assign new ordinal names.
        if (field.unnamed)
            field.name.clear();
        if (!field.name.empty() && !originals.insert(field.name).second)
            throw Diagnostic(field.span, "duplicate field '" + field.name + "'");
    }
    std::size_t ordinal = 0;
    for (auto& field : fields) {
        ++ordinal;
        if (field.name.empty()) {
            field.unnamed = true;
            field.name = type_stem(field.type) + std::to_string(ordinal);
            while (!originals.insert(field.name).second)
                field.name += '_';
        }
        auto base = field.name;
        if (c_reserved().count(base) || reserved.count(base))
            base += '_';
        field.c_name = base;
        for (std::size_t suffix = 2; !used.insert(field.c_name).second; ++suffix)
            field.c_name = base + '_' + std::to_string(suffix);
    }
}

template <class Def>
auto field_lists(Def& definition)
{
    using FieldsPointer = decltype(&definition.fields);
    std::vector<FieldsPointer> result;
    if (definition.kind == DefinitionKind::product) {
        result.push_back(&definition.fields);
    } else {
        result.push_back(&definition.attributes);
        for (auto& constructor : definition.constructors)
            result.push_back(&constructor.fields);
    }
    return result;
}

bool enabled(const Options& options, const char* name)
{
    const auto value = options.find(name);
    return value != options.end() && value->second == "1";
}

struct Dependency {
    std::size_t target;
    const SourceSpan* where;
};

void check_product_cycles(const Schema& schema,
                          const std::vector<std::vector<Dependency>>& dependencies)
{
    // Match the legacy diagnostic path and location: depth-first search
    // in field order, rooted only at products. Avoid recursion on schema
    // input (a long acyclic definition chain is valid).
    for (std::size_t root = 0; root < schema.definitions.size(); ++root) {
        if (schema.definitions[root].kind != DefinitionKind::product)
            continue;
        struct Frame { std::size_t id, edge; };
        std::vector<Frame> path{{root, 0}};
        std::vector<bool> seen(schema.definitions.size());
        seen[root] = true;
        while (!path.empty()) {
            auto& frame = path.back();
            const auto& edges = dependencies[frame.id];
            if (frame.edge == edges.size()) {
                path.pop_back();
                continue;
            }
            const auto& edge = edges[frame.edge++];
            if (edge.target == root) {
                std::string message = "recursive product type: ";
                for (const auto& ancestor : path)
                    message += schema.definitions[ancestor.id].name + " -> ";
                message += schema.definitions[root].name;
                throw Diagnostic(*edge.where, std::move(message));
            }
            if (!seen[edge.target]) {
                seen[edge.target] = true;
                path.push_back({edge.target, 0});
            }
        }
    }
}

} // namespace

SourceMap::SourceMap(std::string_view file, std::string_view text)
    : file_(file), size_(text.size()), lines_{0}
{
    for (std::size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
            lines_.push_back(i + 1);
}

SourcePos SourceMap::position(std::size_t offset) const
{
    if (offset > size_)
        throw std::out_of_range("source offset outside input");
    const auto next = std::upper_bound(lines_.begin(), lines_.end(), offset);
    return {offset, static_cast<std::size_t>(next - lines_.begin()),
            offset - *(next - 1) + 1};
}

SourceSpan SourceMap::span(std::size_t begin, std::size_t end) const
{
    if (begin > end)
        throw std::out_of_range("source span is reversed");
    return {file_, position(begin), position(end)};
}

Diagnostic::Diagnostic(SourceSpan where, std::string text)
    : std::runtime_error(diagnostic_text(where, text)),
      span(std::move(where)), message(std::move(text))
{}

const std::map<std::string, std::string>& scalar_c_types()
{
    static const std::map<std::string, std::string> values{
        {"int", "intmax_t"}, {"uint", "uintmax_t"}, {"bool", "bool"},
        {"real", "long double"}, {"size", "ssize_t"}, {"usize", "size_t"},
    };
    return values;
}

const std::map<std::string, std::string>& builtin_c_types()
{
    static const std::map<std::string, std::string> values{
        {"list", "ASDLVector"}, {"array", "ASDLFixedArray"}, {"map", "ASDLHashMap"},
        {"port", "ASDLPort"}, {"path", "ASDLPath"}, {"buffer", "ASDLByteBuffer"},
        {"sexpr", "ASDLSexpr"}, {"regexp", "ASDLRegexp"}, {"parsec", "ASDLParsec"},
        {"event", "ASDLEventHandler"},
    };
    return values;
}

const std::set<std::string>& primitives()
{
    static const auto values = [] {
        std::set<std::string> names{"string", "identifier"};
        for (const auto& entry : scalar_c_types())
            names.insert(entry.first);
        for (const auto& entry : builtin_c_types())
            names.insert(entry.first);
        return names;
    }();
    return values;
}

std::string type_stem(std::string_view type)
{
    std::string stem;
    bool separator = false;
    for (const char c : type) {
        if (letter(c) || digit(c) || c == '_') {
            stem += c;
            separator = false;
        } else if (!separator) {
            stem += '_';
            separator = true;
        }
    }
    if (!stem.empty() && stem.back() == '_')
        stem.pop_back();
    return stem;
}

std::string c_type(const Schema& schema, std::string_view type)
{
    return schema.prefix + type_stem(type) + "_t";
}

std::string field_type(const Schema& schema, const Field& field)
{
    return schema.prefix + type_stem(field.type) +
        (field.qualifier == '*' || field.qualifier == '+' ? "_seq" :
         field.qualifier == '?' ? "_opt" : "_t");
}

std::string tag_name(const Schema& schema, const Definition& definition,
                     const Constructor& constructor)
{
    return schema.prefix + constructor.name + (definition.is_enum ? "" : "_kind");
}

std::vector<std::string> type_dependencies(const Schema& schema, std::string_view type)
{
    struct Frame { std::string_view type; bool exit; };
    std::vector<Frame> pending{{type, false}};
    std::set<std::string_view> active;
    std::vector<std::string> output;
    while (!pending.empty()) {
        auto frame = pending.back();
        pending.pop_back();
        if (frame.exit) {
            active.erase(frame.type);
            continue;
        }
        auto generic = schema.generics.find(std::string(frame.type));
        if (generic == schema.generics.end()) {
            output.emplace_back(frame.type);
            continue;
        }
        if (!active.insert(frame.type).second)
            throw Diagnostic(generic->second.span,
                             "cyclic generic type '" + std::string(frame.type) + "'");
        output.push_back(generic->second.kind);
        pending.push_back({frame.type, true});
        const auto& args = generic->second.args;
        for (auto arg = args.rbegin(); arg != args.rend(); ++arg)
            if (arg->empty() || !std::all_of(arg->begin(), arg->end(), digit))
                pending.push_back({*arg, false});
    }
    return output;
}

std::vector<std::string> primitive_types(const Schema& schema)
{
    std::vector<std::string> names{"int", "string", "identifier"};
    for (const auto& name : schema.used_builtins)
        if (name != "int" && name != "string" && name != "identifier" &&
            name != "list" && name != "array" && name != "map")
            names.push_back(name);
    return names;
}

std::vector<std::string> all_types(const Schema& schema)
{
    auto names = primitive_types(schema);
    for (const auto& generic : schema.generics)
        names.push_back(generic.first);
    for (const auto& definition : schema.definitions)
        names.push_back(definition.name);
    return names;
}

void resolve_options(Schema& schema, const Options& overrides)
{
    Options options{
        {"enums", "1"}, {"line-directives", "1"}, {"header-only", "1"}, {"builtins", "1"},
    };
    for (const auto& entry : schema.options)
        options[entry.first] = entry.second;
    for (const auto& entry : overrides)
        options[entry.first] = entry.second;
    if (overrides.count("source")) {
        if (enabled(overrides, "header-only"))
            throw std::runtime_error("--source and --header-only cannot be combined on the command line");
        options["header-only"] = "0";
    }
    for (const auto* name : {"enums", "line-directives", "header-only", "builtins"})
        if (options.at(name) != "0" && options.at(name) != "1")
            throw std::runtime_error("invalid boolean value for --" + std::string(name));
    for (const auto* name : {"header", "source", "visitor", "listener", "include",
            "prefix", "guard", "schema", "builtins-header", "omit-builtin", "cpp-header", "dot"})
        if (options.count(name) && options.at(name).empty())
            throw std::runtime_error("--" + std::string(name) + " requires a nonempty value");
    if (enabled(options, "header-only")) {
        options.erase("source");
    } else if (!options.count("source") || !options.count("header") ||
               options.at("source") == "-" || options.at("header") == "-") {
        throw std::runtime_error("split output requires --source FILE and --header FILE");
    }
    for (const auto* name : {"visitor", "listener", "builtins-header"})
        if (options.count(name) && options.at(name) == "-")
            throw std::runtime_error("--" + std::string(name) + " requires a filename, not stdout");

    std::set<std::string> omitted;
    if (const auto entry = options.find("omit-builtin"); entry != options.end()) {
        std::string_view list = entry->second;
        // Like Perl split(), ignore empty trailing entries, but not empty
        // entries between names.
        while (!list.empty() && list.back() == ',')
            list.remove_suffix(1);
        while (!list.empty()) {
            const auto comma = list.find(',');
            const auto name = list.substr(0, comma);
            const auto& types = builtin_c_types();
            const auto builtin = std::find_if(types.begin(), types.end(), [&](const auto& pair) {
                return pair.first == name || pair.second == name;
            });
            if (builtin == types.end())
                throw std::runtime_error("unknown builtin '" + std::string(name) + "'");
            omitted.insert(builtin->first);
            if (comma == std::string_view::npos)
                break;
            list.remove_prefix(comma + 1);
        }
    }

    auto prefix = options.count("prefix") ? options.at("prefix") :
        (schema.module.empty() ? "asdl" : lower(schema.module));
    if (!identifier(prefix))
        throw std::runtime_error("invalid C prefix '" + prefix + "'");
    if (prefix.back() != '_')
        prefix += '_';
    auto uppercase = upper(prefix);
    auto guard = options.count("guard") ? options.at("guard") : uppercase + "AST_H";
    if (!identifier(guard))
        throw std::runtime_error("invalid C header guard '" + guard + "'");
    schema.prefix = std::move(prefix);
    schema.upper = std::move(uppercase);
    schema.guard = std::move(guard);
    schema.omitted_builtins = std::move(omitted);
    schema.options = std::move(options);
}

void validate(Schema& schema)
{
    resolve_options(schema);
    schema.types.clear();
    schema.constructors.clear();
    schema.used_builtins.clear();
    schema.type_ids.clear();
    schema.type_names.clear();
    if (schema.definitions.empty())
        throw Diagnostic({schema.file, {}, {}}, "no ASDL definitions found");

    for (std::size_t i = 0; i < schema.definitions.size(); ++i) {
        auto& definition = schema.definitions[i];
        if (primitives().count(definition.name))
            throw Diagnostic(definition.span, "cannot redefine primitive '" + definition.name + "'");
        if (!schema.types.emplace(definition.name, i).second)
            throw Diagnostic(definition.span, "duplicate type '" + definition.name + "'");
        for (std::size_t j = 0; j < definition.constructors.size(); ++j) {
            auto& constructor = definition.constructors[j];
            if (!schema.constructors.emplace(constructor.name, ConstructorIndex{i, j}).second)
                throw Diagnostic(constructor.span, "duplicate constructor '" + constructor.name + "'");
            constructor.tag = j + 1;
        }
        definition.is_enum = enabled(schema.options, "enums") &&
            definition.kind == DefinitionKind::sum && definition.attributes.empty() &&
            std::all_of(definition.constructors.begin(), definition.constructors.end(),
                        [](const Constructor& c) { return c.fields.empty(); });
        name_fields(definition.attributes, {"kind", "v"});
        if (definition.kind == DefinitionKind::product)
            name_fields(definition.fields, {});
        else
            for (auto& constructor : definition.constructors)
                name_fields(constructor.fields, {});
    }

    const auto intern = [&](const std::string& name) {
        auto [entry, inserted] = schema.type_ids.emplace(name, schema.type_names.size());
        if (inserted)
            schema.type_names.push_back(name);
        return entry->second;
    };
    for (const auto& name : primitives())
        intern(name);
    for (const auto& entry : schema.generics)
        intern(entry.first);
    for (const auto& definition : schema.definitions)
        intern(definition.name);

    std::vector<std::vector<Dependency>> edges(schema.definitions.size());
    std::map<std::string, std::vector<std::string>> dependency_cache;
    for (std::size_t i = 0; i < schema.definitions.size(); ++i) {
        for (auto* list : field_lists(schema.definitions[i])) {
            for (auto& field : *list) {
                auto [entry, inserted] = dependency_cache.try_emplace(field.type);
                if (inserted)
                    entry->second = type_dependencies(schema, field.type);
                for (const auto& dependency : entry->second) {
                    if (primitives().count(dependency)) {
                        schema.used_builtins.insert(dependency);
                    } else if (const auto target = schema.types.find(dependency);
                               target != schema.types.end()) {
                        edges[i].push_back({target->second, &field.span});
                    } else {
                        throw Diagnostic(field.span, "undefined type '" + dependency + "'");
                    }
                }
                field.type_id = schema.type_ids.at(field.type);
            }
        }
    }
    check_product_cycles(schema, edges);
}

} // namespace asdl
