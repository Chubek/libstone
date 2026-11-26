#include <asdl/codegen.hpp>
#include "codegen_runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace asdl {
namespace {

namespace fs = std::filesystem;
namespace rt = codegen_detail;
using Strings = std::vector<std::string>;
using Text = std::string;
using View = std::string_view;

bool primitive(const Text& name)
{
    return primitives().contains(name);
}

Text quote(View value, bool json = false)
{
    Text out = "\"";
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 32 || ch == 127) {
                if (json) {
                    constexpr View hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[ch >> 4];
                    out += hex[ch & 15];
                } else {
                    out += '\\';
                    out += char('0' + (ch >> 6));
                    out += char('0' + ((ch >> 3) & 7));
                    out += char('0' + (ch & 7));
                }
            } else if (ch == '?' && !json) {
                // Also safe when a C11 implementation enables trigraphs.
                out += "\\?";
            } else {
                out += char(ch);
            }
        }
    }
    return out + '"';
}

Text join(const Strings& list, View separator)
{
    Text out;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += separator;
        out += list[i];
    }
    return out;
}

Text relative_include(const Text& target, const Text& output)
{
    auto dir = fs::path(output).parent_path();
    if (dir.empty()) dir = ".";
    return fs::absolute(target).lexically_normal()
        .lexically_relative(fs::absolute(dir).lexically_normal()).generic_string();
}

Text include_line(const Text& name)
{
    if (name.empty() || name.find_first_of(View{"\"\\\r\n\0", 5}) != Text::npos
        || name.find("??") != Text::npos)
        throw std::runtime_error("invalid quoted C include path '" + name + "'");
    return "#include \"" + name + "\"\n";
}

class Emitter {
public:
    explicit Emitter(const Schema& schema) : s(schema), p(s.prefix), u(s.upper)
    {
        for (const auto& d : s.definitions) types.emplace(d.name, &d);
    }
    GeneratedOutputs emit();

private:
    using Operation = std::function<Text(const Text&, const Text&, const Text&)>;
    const Schema& s;
    const Text& p;
    const Text& u;
    std::map<Text, const Definition*> types;
    std::map<Text, Text> symbols;
    std::set<Text> c_tags;
    std::set<Text> c_macros;
    std::vector<std::pair<Text, Text>> functions;

    Text opt(const Text& key, Text fallback = {}) const
    {
        const auto pos = s.options.find(key);
        return pos == s.options.end() ? std::move(fallback) : pos->second;
    }
    bool enabled(const Text& key) const { return opt(key, "1") == "1"; }
    Text ctype(const Text& name) const { return c_type(s, name); }
    Text ftype(const Field& f) const { return field_type(s, f); }
    Text tag(const Definition& d, const Constructor& c) const
    {
        return tag_name(s, d, c);
    }
    Text member(const Constructor& c) const;
    Text apply_template(View source) const;
    void symbol(const Text& name, const Text& meaning);
    void macro_symbol(const Text& name, const Text& meaning);
    void tag_symbol(const Text& name);
    void function(Text signature, Text body);
    Text shallow_test(const Text& type, const Text& value) const;
    Text fields(const std::vector<Field>& fields, const Text& indent) const;
    Text runtime_types();
    Text ast_types();
    Text inventory();
    void runtime_functions();
    void collection_functions();
    void generic_functions();
    Text field_operation(const Field& f, const Text& value, const Text& invalid,
                         const Operation& operation, const Text& indent = "    ") const;
    Text value_operation(const Text& invalid, const Operation& operation, const Text& type,
                         const Text& value, const Text& indent) const;
    void builders();
    void validators();
    Text injected(const std::optional<Fragment>& fragment, const Text& output) const;
    Text header_include(const Text& destination) const;
    Text builtins() const;
    Text visitor_callback(const Text& hook, const Text& value, const Text& indent,
                          const Text& skip = {}) const;
    Text visitor(const Text& destination) const;
    Text schema_json() const;
    Text cpp20() const;
};

Text Emitter::member(const Constructor& c) const
{
    // These macro tokens can occur in constructor names (which are uppercase).
    // The C public constructor/tag is prefixed; only the union member is bare.
    static const std::set<Text> macros{
        "NULL", "EOF", "EXIT_SUCCESS", "EXIT_FAILURE", "RAND_MAX", "MB_CUR_MAX",
        "SIZE_MAX", "PTRDIFF_MIN", "PTRDIFF_MAX", "SIG_ATOMIC_MIN", "SIG_ATOMIC_MAX",
        "WCHAR_MIN", "WCHAR_MAX", "WINT_MIN", "WINT_MAX",
    };
    static const std::set<Text> widths{"8", "16", "32", "64", "LEAST8", "LEAST16", "LEAST32",
        "LEAST64", "FAST8", "FAST16", "FAST32", "FAST64", "PTR", "MAX"};
    bool reserved = macros.contains(c.name);
    for (const auto& width : widths)
        reserved = reserved || c.name == "INT" + width + "_MIN"
            || c.name == "INT" + width + "_MAX" || c.name == "UINT" + width + "_MAX";
    if (!reserved) return c.name;
    Text escaped = c.name + '_';
    const auto taken = [&](const Text& name) {
        for (const auto& d : s.definitions)
            for (const auto& other : d.constructors)
                if (other.name == name) return true;
        return false;
    };
    while (taken(escaped)) escaped += '_';
    return escaped;
}

Text Emitter::apply_template(View source) const
{
    Text out;
    size_t start = 0;
    while (start < source.size()) {
        auto pos = source.find('@', start);
        if (pos == View::npos) {
            out += source.substr(start);
            break;
        }
        out += source.substr(start, pos - start);
        const auto token = source.substr(pos, 3);
        if (token == "@P@" || token == "@U@") {
            out += token == "@P@" ? p : u;
            start = pos + 3;
        } else {
            out += '@';
            start = pos + 1;
        }
    }
    return out;
}

void Emitter::symbol(const Text& name, const Text& meaning)
{
    const auto [pos, added] = symbols.emplace(name, meaning);
    if (!added)
        throw std::runtime_error(s.file + ": generated C symbol '" + name + "' conflicts: "
                                 + pos->second + " / " + meaning);
}

void Emitter::macro_symbol(const Text& name, const Text& meaning)
{
    symbol(name, meaning);
    c_macros.insert(name);
    if (c_tags.contains(name))
        throw std::runtime_error(s.file + ": generated C symbol '" + name + "' conflicts: C tag / " + meaning);
}

void Emitter::tag_symbol(const Text& name)
{
    if (!c_tags.insert(name).second || c_macros.contains(name))
        throw std::runtime_error(s.file + ": generated C tag '" + name + "' conflicts with another tag or macro");
}

void Emitter::function(Text signature, Text body)
{
    auto end = signature.find('(');
    auto start = signature.find_last_of(" *", end);
    symbol(signature.substr(start + 1, end - start - 1), "function");
    functions.emplace_back(std::move(signature), std::move(body));
}

Text Emitter::shallow_test(const Text& type, const Text& value) const
{
    if (const auto found = s.generics.find(type); found != s.generics.end()) {
        const auto& kind = found->second.kind;
        const auto& args = found->second.args;
        const auto v = "(" + value + ")";
        if (kind == "map")
            return "(" + v + ".key_size == sizeof(" + ctype(args[0])
                + ") && " + v + ".value_size == sizeof(" + ctype(args[1])
                + ") && " + v + ".hash && " + v + ".length <= " + v + ".capacity"
                + " && " + v + ".capacity <= SIZE_MAX / sizeof(*" + v + ".entries) && (!"
                + v + ".capacity || " + v + ".entries))";
        const auto element = kind == "array" && args.size() == 1 ? "unsigned char" : ctype(args[0]);
        return "(" + v + ".element_size == sizeof(" + element + ") && "
            + v + ".length <= SIZE_MAX / sizeof(" + element + ") && (!" + v + ".length || "
            + v + ".data) && " + v + ".length "
            + (kind == "array" ? "== " + args.back() : "<= " + v + ".capacity") + ")";
    }
    if (scalar_c_types().contains(type) || builtin_c_types().contains(type)) return "1";
    if (type == "string") return "((" + value + ").length == 0 || (" + value + ").data != NULL)";
    if (type == "identifier" || !types.at(type)->is_enum) return "((" + value + ") != NULL)";
    const auto& d = *types.at(type);
    return "((" + value + ") >= " + tag(d, d.constructors.front()) + " && (" + value + ") <= "
        + tag(d, d.constructors.back()) + ")";
}

Text Emitter::fields(const std::vector<Field>& names, const Text& indent) const
{
    Text out;
    for (const auto& f : names)
        out += indent + ftype(f) + " " + f.c_name + "; /* ASDL: " + f.name + " */\n";
    return out;
}

Text Emitter::runtime_types()
{
    for (auto name : {"error", "visit_result", "block", "intern", "arena", "string_t", "identifier_t"})
        symbol(p + name, "runtime type");
    for (auto name : {"error", "visit_result", "block", "intern", "arena", "string"})
        tag_symbol(p + name);
    Text out;
    for (const auto& name : primitive_types(s)) {
        if (name == "string" || name == "identifier") continue;
        const auto t = scalar_c_types().contains(name) ? scalar_c_types().at(name) : "struct " + builtin_c_types().at(name);
        symbol(ctype(name), "builtin type");
        out += "typedef " + t + " " + ctype(name) + ";\n";
    }
    for (auto name : {"OK", "OOM", "INVALID", "OVERFLOW", "WALK_CONTINUE", "WALK_SKIP", "WALK_STOP", "WALK_ERROR"})
        symbol(u + name, "runtime constant");
    for (auto name : {"MALLOC", "FREE", "VISITOR_NAME", "VISITOR_CONTEXT", "VISITOR_MAX_DEPTH",
                      "PRIVATE_CAT_I", "PRIVATE_CAT", "PRIVATE_FN", "PRIVATE_DEFAULT_CONTEXT", "PRIVATE_DEFAULT_DEPTH"})
        macro_symbol(u + name, "configuration macro");
    return out + apply_template(rt::runtime_types);
}

Text Emitter::ast_types()
{
    Text out = "/* ---- ASDL types (forward declarations permit recursive sums) ---- */\n";
    for (const auto& d : s.definitions) {
        const auto t = ctype(d.name);
        symbol(t, "type " + d.name);
        if (d.kind == DefinitionKind::sum) {
            const auto kind = p + d.name + "_kind";
            symbol(kind, "sum tag type");
            tag_symbol(p + d.name + "_tag");
            out += "typedef enum " + p + d.name + "_tag {\n";
            for (const auto& c : d.constructors) {
                const auto name = tag(d, c);
                symbol(name, "tag " + c.name);
                out += "    " + name + " = " + std::to_string(c.tag) + ",\n";
            }
            out += "} " + kind + ";\n";
            if (d.is_enum) out += "typedef " + kind + " " + t + ";\n";
        }
        if (!d.is_enum) {
            tag_symbol(p + d.name + "_node");
            out += "typedef struct " + p + d.name + "_node *" + t + ";\n";
        }
    }
    for (const auto& [name, spec] : s.generics) {
        symbol(ctype(name), "generic " + name);
        out += "typedef struct " + builtin_c_types().at(spec.kind) + " " + ctype(name) + ";\n";
    }
    out += "\n/* Typed collections and explicit presence, including scalar options. */\n";
    for (const auto& name : all_types(s)) {
        auto t = ctype(name), stem = p + type_stem(name);
        for (auto suffix : {"_seq", "_opt"}) symbol(stem + suffix, "collection type");
        out += "typedef struct { size_t length; " + t + " *items; } " + stem + "_seq;\n"
            + "typedef struct { int has_value; " + t + " value; } " + stem + "_opt;\n";
    }
    for (const auto& d : s.definitions) {
        if (d.is_enum) continue;
        out += "\nstruct " + p + d.name + "_node {\n";
        if (d.kind == DefinitionKind::product) {
            out += fields(d.fields, "    ");
        } else {
            out += "    " + p + d.name + "_kind kind;\n" + fields(d.attributes, "    ");
            if (std::ranges::any_of(d.constructors, [](const auto& c) { return !c.fields.empty(); })) {
                out += "    union {\n";
                for (const auto& c : d.constructors)
                    if (!c.fields.empty())
                        out += "        struct {\n" + fields(c.fields, "            ")
                            + "        } " + member(c) + ";\n";
                out += "    } v;\n";
            }
        }
        out += "};\n";
    }
    return out + "\n";
}

Text Emitter::inventory()
{
    Strings defs, ctors;
    for (const auto& d : s.definitions) {
        defs.push_back("X(" + d.name + ", " + ctype(d.name) + ", "
            + (d.is_enum ? "ENUM" : d.kind == DefinitionKind::product ? "PRODUCT" : "SUM") + ")");
        for (const auto& c : d.constructors)
            ctors.push_back("X(" + d.name + ", " + c.name + ", " + std::to_string(c.tag) + ")");
    }
    for (auto name : {"TYPE_COUNT", "CONSTRUCTOR_COUNT", "FOR_EACH_TYPE", "FOR_EACH_CONSTRUCTOR"})
        macro_symbol(u + name, "inventory macro");
    Text out = "#define " + u + "TYPE_COUNT " + std::to_string(defs.size()) + "\n#define "
        + u + "CONSTRUCTOR_COUNT " + std::to_string(ctors.size()) + "\n";
    out += "#define " + u + "FOR_EACH_TYPE(X)";
    for (const auto& entry : defs) out += " \\\n    " + entry;
    out += "\n#define " + u + "FOR_EACH_CONSTRUCTOR(X)";
    for (const auto& entry : ctors) out += " \\\n    " + entry;
    return out + "\n\n";
}

void Emitter::runtime_functions()
{
    const auto text = apply_template(rt::runtime_functions);
    constexpr View marker = "FUNCTION ";
    for (size_t start = 0; start < text.size();) {
        const auto sig = start + marker.size();
        const auto body = text.find("\n{\n", sig);
        const auto end = text.find("\n}\n", body + 2);
        if (text.compare(start, marker.size(), marker) || body == Text::npos || end == Text::npos)
            throw std::logic_error("invalid built-in runtime function template");
        function(text.substr(sig, body - sig), text.substr(body + 3, end - body - 2));
        start = end + 3;
    }
}

void Emitter::collection_functions()
{
    for (const auto& name : all_types(s)) {
        const auto stem = p + type_stem(name), t = ctype(name);
        function(stem + "_opt " + stem + "_none(void)",
                 "    " + stem + "_opt result = {0};\n    return result;\n");
        function(stem + "_opt " + stem + "_some(" + t + " value)",
                 "    " + stem + "_opt result = {1, value};\n    return result;\n");
        function(stem + "_seq " + stem + "_seq_new(" + p + "arena *arena, size_t length)",
                 "    " + stem + "_seq result = {0, NULL};\n"
                 "    size_t i;\n"
                 "    if (!arena || arena->error != " + u + "OK) return result;\n"
                 "    if (length > SIZE_MAX / sizeof(*result.items)) {\n"
                 "        " + p + "set_error(arena, " + u + "OVERFLOW);\n"
                 "        return result;\n"
                 "    }\n"
                 "    if (!length) return result;\n"
                 "    result.items = (" + t + " *)" + p + "arena_alloc(arena, length * sizeof(*result.items));\n"
                 "    if (!result.items) return result;\n"
                 "    result.length = length;\n"
                 "    for (i = 0; i < length; ++i) result.items[i] = (" + t + "){0};\n"
                 "    return result;\n");
        const auto check = shallow_test(name, "items[i]");
        Text checks;
        if (check != "1") {
            checks = "    for (i = 0; i < length; ++i) {\n"
                "        if (!(" + check + ")) { " + p + "set_error(arena, " + u + "INVALID); return result; }\n"
                "    }\n";
        }
        function(stem + "_seq " + stem + "_seq_copy(" + p + "arena *arena, const " + t + " *items, size_t length)",
                 "    " + stem + "_seq result = {0, NULL};\n"
                 + (check == "1" ? "" : "    size_t i;\n")
                 + "    if (!arena || arena->error != " + u + "OK) return result;\n"
                 "    if (length > SIZE_MAX / sizeof(*result.items)) {\n"
                 "        " + p + "set_error(arena, " + u + "OVERFLOW);\n"
                 "        return result;\n"
                 "    }\n"
                 "    if (length && !items) { " + p + "set_error(arena, " + u + "INVALID); return result; }\n"
                 + checks + "    result = " + stem + "_seq_new(arena, length);\n"
                 "    if (result.items) memcpy(result.items, items, length * sizeof(*items));\n"
                 "    return result;\n");
    }
}

void Emitter::generic_functions()
{
    for (const auto& [name, spec] : s.generics) {
        const auto& kind = spec.kind;
        const auto& args = spec.args;
        const auto stem = p + type_stem(name), t = ctype(name);
        const auto e = kind == "array" && args.size() == 1 ? "unsigned char" : ctype(args[0]);
        if (kind == "map") {
            const auto v = ctype(args[1]), hash = type_stem(args[0]) + "_hashfn";
            function("size_t " + stem + "_hash(const void *key)",
                     "    " + e + " value;\n    memcpy(&value, key, sizeof(value));\n"
                     "    return (size_t)" + hash + "(value);\n");
            function(t + " " + stem + "_new(int (*equal)(const void *, const void *))",
                     "    " + t + " result;\n    ASDLHashMap_init(&result, sizeof(" + e + "), sizeof("
                     + v + "), " + stem + "_hash, equal);\n    return result;\n");
            function("bool " + stem + "_set(" + t + " *map, " + e + " key, " + v + " value)",
                     "    return map && map->key_size == sizeof(key) && map->value_size == sizeof(value)\n"
                     "        && ASDLHashMap_set(map, &key, &value);\n");
            function(v + " *" + stem + "_get(const " + t + " *map, " + e + " key)",
                     "    return map && map->key_size == sizeof(key) && map->value_size == sizeof(" + v + ")\n"
                     "        ? (" + v + " *)ASDLHashMap_get(map, &key) : NULL;\n");
            function("bool " + stem + "_remove(" + t + " *map, " + e + " key)",
                     "    return map && map->key_size == sizeof(key) && map->value_size == sizeof(" + v + ")\n"
                     "        && ASDLHashMap_remove(map, &key);\n");
        } else {
            const auto& r = builtin_c_types().at(kind);
            function(t + " " + stem + "_new(void)",
                     "    " + t + " result = {0};\n    " + r + "_init(&result, sizeof(" + e + ")"
                     + (kind == "list" ? "" : ", " + args.back()) + ");\n    return result;\n");
            function(e + " *" + stem + "_at(" + t + " *collection, size_t index)",
                     "    return collection && collection->element_size == sizeof(" + e + ")\n"
                     "        ? (" + e + " *)" + r + "_at(collection, index) : NULL;\n");
            function("bool " + stem + "_set(" + t + " *collection, size_t index, " + e + " value)",
                     "    return collection && collection->element_size == sizeof(value)\n"
                     "        && " + r + "_set(collection, index, &value);\n");
            if (kind == "list")
                function("bool " + stem + "_push(" + t + " *collection, " + e + " value)",
                         "    return collection && collection->element_size == sizeof(value)\n"
                         "        && ASDLVector_push(collection, &value);\n");
        }
    }
}

Text Emitter::field_operation(const Field& f, const Text& value, const Text& invalid,
                              const Operation& operation, const Text& indent) const
{
    const auto v = "(" + value + ")";
    if (f.qualifier == '?')
        return indent + "if (" + v + ".has_value != 0 && " + v + ".has_value != 1) { " + invalid + " }\n"
            + indent + "if (" + v + ".has_value) {\n"
            + value_operation(invalid, operation, f.type, v + ".value", indent + "    ")
            + indent + "}\n";
    if (f.qualifier == '*' || f.qualifier == '+')
        return (f.qualifier == '+' ? indent + "if (!" + v + ".length) { " + invalid + " }\n" : "")
            + indent + "if (" + v + ".length && !" + v + ".items) { " + invalid + " }\n"
            + indent + "if (" + v + ".length > SIZE_MAX / sizeof(*" + v + ".items)) { " + invalid + " }\n"
            + indent + "{\n" + indent + "    size_t index;\n"
            + indent + "    for (index = 0; index < " + v + ".length; ++index) {\n"
            + value_operation(invalid, operation, f.type, v + ".items[index]", indent + "        ")
            + indent + "    }\n" + indent + "}\n";
    return value_operation(invalid, operation, f.type, value, indent);
}

Text Emitter::value_operation(const Text& invalid, const Operation& operation, const Text& type,
                              const Text& value, const Text& i) const
{
    const auto pos = s.generics.find(type);
    if (pos == s.generics.end()) return operation(type, value, i);
    const auto& spec = pos->second;
    const auto v = "(" + value + ")";
    Text out = i + "if (!(" + shallow_test(type, value) + ")) { " + invalid + " }\n" + i + "{\n";
    if (spec.kind == "array" && spec.args.size() == 1) return out + i + "}\n";
    const bool map = spec.kind == "map";
    if (map) out += i + "    size_t occupied = 0;\n";
    out += i + "    size_t element;\n" + i + "    for (element = 0; element < "
        + v + (map ? ".capacity" : ".length") + "; ++element) {\n";
    if (map) {
        out += i + "        if (" + v + ".entries[element].state > 2) { " + invalid + " }\n"
            + i + "        if (!(" + v + ".entries[element].state == 1)) continue;\n"
            + i + "        ++occupied;\n";
    }
    for (size_t n = 0; n < (map ? 2u : 1u); ++n) {
        const auto t = ctype(spec.args[n]), item = "item_" + std::to_string(i.size());
        const auto data = map ? v + ".entries[element]." + (n ? "value" : "key")
            : "(const unsigned char *)" + v + ".data + element * sizeof(" + t + ")";
        out += i + "        {\n" + i + "            " + t + " " + item + ";\n";
        if (map) out += i + "            if (!(" + data + ")) { " + invalid + " }\n";
        out += i + "            memcpy(&" + item + ", " + data + ", sizeof(" + item + "));\n"
            + i + "            (void)" + item + ";\n"
            + value_operation(invalid, operation, spec.args[n], item, i + "            ")
            + i + "        }\n";
    }
    out += i + "    }\n";
    if (map) out += i + "    if (occupied != " + v + ".length) { " + invalid + " }\n";
    return out + i + "}\n";
}

void Emitter::builders()
{
    const auto invalid = p + "set_error(arena, " + u + "INVALID); return NULL;";
    const Operation leaf = [&](const Text& type, const Text& value, const Text& indent) -> Text {
        const auto test = shallow_test(type, value);
        return test == "1" ? "" : indent + "if (!(" + test + ")) { " + invalid + " }\n";
    };
    for (const auto& d : s.definitions) {
        if (d.is_enum) continue;
        const auto t = ctype(d.name);
        const auto add = [&](const Text& name, const std::vector<Field>& payload, const Constructor* c) {
            Strings params{p + "arena *arena"};
            Text assigns, checks;
            const auto args = [&](const std::vector<Field>& list, const Text& arg, const Text& path) {
                for (const auto& f : list) {
                    const auto param = arg + f.c_name;
                    params.push_back(ftype(f) + " " + param);
                    assigns += "    node->" + path + f.c_name + " = " + param + ";\n";
                    checks += field_operation(f, param, invalid, leaf);
                }
            };
            args(payload, "arg_", c ? "v." + member(*c) + "." : "");
            args(d.attributes, "attr_", "");
            auto body = "    " + t + " node;\n    if (!arena || arena->error != " + u + "OK) return NULL;\n"
                + checks + "    node = (" + t + ")" + p + "arena_alloc(arena, sizeof(*node));\n"
                "    if (!node) return NULL;\n    *node = (struct " + p + d.name + "_node){0};\n";
            if (c) body += "    node->kind = " + tag(d, *c) + ";\n";
            function(t + " " + p + name + "(" + join(params, ", ") + ")", body + assigns + "    return node;\n");
        };
        if (d.kind == DefinitionKind::product) add("make_" + d.name, d.fields, nullptr);
        else for (const auto& c : d.constructors) add(c.name, c.fields, &c);
    }
}

void Emitter::validators()
{
    const Operation leaf = [&](const Text& type, const Text& value, const Text& indent) -> Text {
        const auto test = primitive(type) ? shallow_test(type, value) : p + "validate_" + type + "(" + value + ", depth - 1)";
        return test == "1" ? "" : indent + "if (!(" + test + ")) return 0;\n";
    };
    for (const auto& d : s.definitions) {
        Text body = "    if (!depth) return 0;\n", names;
        if (!d.is_enum) body += "    if (!node) return 0;\n";
        if (d.kind == DefinitionKind::product) {
            for (const auto& f : d.fields)
                body += field_operation(f, "node->" + f.c_name, "return 0;", leaf);
            names = "    return node ? " + quote(d.name) + " : NULL;\n";
        } else {
            const Text start = d.is_enum ? "    switch (node) {\n" : "    switch (node->kind) {\n";
            body += start;
            if (!d.is_enum) names += "    if (!node) return NULL;\n";
            names += start;
            for (const auto& c : d.constructors) {
                const auto label = "    case " + tag(d, c) + ":";
                body += label + "\n";
                names += label + " return " + quote(c.name) + ";\n";
                for (const auto& f : d.attributes)
                    body += field_operation(f, "node->" + f.c_name, "return 0;", leaf, "        ");
                for (const auto& f : c.fields)
                    body += field_operation(f, "node->v." + member(c) + "." + f.c_name, "return 0;", leaf, "        ");
                body += "        break;\n";
            }
            body += "    default: return 0;\n    }\n";
            names += "    default: return NULL;\n    }\n";
        }
        const auto t = ctype(d.name);
        function("int " + p + "validate_" + d.name + "(" + t + " node, size_t depth)", body + "    return 1;\n");
        function("const char *" + p + d.name + "_name(" + t + " node)", names);
    }
}

Text Emitter::injected(const std::optional<Fragment>& fragment, const Text& output) const
{
    if (!fragment || fragment->text.empty()) return {};
    const auto directives = enabled("line-directives");
    Text out = "\n";
    if (directives)
        out += "#line " + std::to_string(fragment->span.begin.line) + " " + quote(s.file) + "\n";
    out += fragment->text;
    if (!out.ends_with('\n')) out += '\n';
    if (directives) out += "#line 1 " + quote(output) + "\n";
    return out;
}

Text Emitter::header_include(const Text& destination) const
{
    if (auto name = opt("include"); !name.empty()) return include_line(name);
    const auto header = opt("header");
    if (header.empty() || header == "-")
        throw std::runtime_error("--visitor/--listener requires --header or --include when the header is written to stdout");
    return include_line(relative_include(header, destination));
}

Text Emitter::builtins() const
{
    if (!enabled("builtins")) return {};
    Text out(rt::common);
    const std::pair<View, View> sections[]{
        {"list", rt::list}, {"array", rt::array}, {"map", rt::map}, {"buffer", rt::buffer},
        {"port", rt::port}, {"path", rt::path}, {"event", rt::event}, {"sexpr", rt::sexpr},
        {"regexp", rt::regexp}, {"parsec", rt::parsec},
    };
    for (const auto& [name, text] : sections)
        if (!s.omitted_builtins.contains(Text(name))) out += text;
    return out;
}

Text Emitter::visitor_callback(const Text& hook, const Text& value, const Text& i, const Text& skip) const
{
    const auto macro = u + hook;
    Text code = "#ifdef " + macro + "\n" + i + "action = " + macro + "(context, " + value + ");\n";
    if (!skip.empty()) {
        code += i + "if (action == " + u + "WALK_SKIP) goto " + skip + ";\n"
            + i + "if (action != " + u + "WALK_CONTINUE) return " + u + "PRIVATE_FN(__result)(action);\n";
    } else {
        code += i + "if (action != " + u + "WALK_CONTINUE && action != " + u + "WALK_SKIP)\n"
            + i + "    return " + u + "PRIVATE_FN(__result)(action);\n";
    }
    return code + "#endif\n";
}

Text Emitter::visitor(const Text& destination) const
{
    auto out = "/* Generated by asdl: intentionally re-includable. */\n"
        + header_include(destination) + "\n" + apply_template(rt::visitor_begin);
    const auto declaration = [&](const Definition& d) {
        return "static inline int " + u + "PRIVATE_FN(__depth_" + d.name + ")(" + ctype(d.name)
            + " node, " + u + "VISITOR_CONTEXT *context, size_t depth)";
    };
    for (const auto& d : s.definitions) out += declaration(d) + ";\n";
    out += '\n';
    const Operation leaf = [&](const Text& type, const Text& value, const Text& i) -> Text {
        if (primitive(type)) {
            const auto check = shallow_test(type, value);
            return (check == "1" ? "" : i + "if (!(" + check + ")) return " + u + "WALK_ERROR;\n")
                + visitor_callback("VALUE_" + type, value, i);
        }
        return i + "action = " + u + "PRIVATE_FN(__depth_" + type + ")(" + value + ", context, depth - 1);\n"
            + i + "if (action != " + u + "WALK_CONTINUE) return action;\n";
    };
    const auto invalid = "return " + u + "WALK_ERROR;";
    for (const auto& d : s.definitions) {
        auto body = "    int action = " + u + "WALK_CONTINUE;\n    (void)action;\n    (void)context;\n"
            "    if (!depth || !" + p + d.name + "_name(node)) " + invalid + "\n"
            + visitor_callback("ENTER_" + d.name, "node", "    ", "leave_type");
        if (d.kind == DefinitionKind::product) {
            for (const auto& f : d.fields)
                body += field_operation(f, "node->" + f.c_name, invalid, leaf);
        } else {
            body += d.is_enum ? "    switch (node) {\n" : "    switch (node->kind) {\n";
            for (const auto& c : d.constructors) {
                const auto label = "leave_ctor_" + std::to_string(c.tag);
                body += "    case " + tag(d, c) + ": {\n" + visitor_callback("VISIT_" + c.name, "node", "        ", label);
                for (const auto& f : d.attributes)
                    body += field_operation(f, "node->" + f.c_name, invalid, leaf, "        ");
                for (const auto& f : c.fields)
                    body += field_operation(f, "node->v." + member(c) + "." + f.c_name, invalid, leaf, "        ");
                body += "#ifdef " + u + "VISIT_" + c.name + "\n    " + label + ": ;\n#endif\n"
                    + visitor_callback("LEAVE_" + c.name, "node", "        ") + "        break;\n    }\n";
            }
            body += "    default: " + invalid + "\n    }\n";
        }
        body += "#ifdef " + u + "ENTER_" + d.name + "\nleave_type: ;\n#endif\n"
            + visitor_callback("LEAVE_" + d.name, "node", "    ") + "    return " + u + "WALK_CONTINUE;\n";
        out += declaration(d) + "\n{\n" + body + "}\n\n";
    }
    for (const auto& d : s.definitions)
        out += "static inline int " + u + "PRIVATE_FN(_" + d.name + ")(" + ctype(d.name) + " node, "
            + u + "VISITOR_CONTEXT *context)\n{\n    return " + u + "PRIVATE_FN(__depth_" + d.name
            + ")(node, context, (size_t)" + u + "VISITOR_MAX_DEPTH);\n}\n\n";
    return out + apply_template(rt::visitor_end);
}

Text Emitter::schema_json() const
{
    using Object = std::map<Text, Text>;
    const auto str = [](View value) { return quote(value, true); };
    const auto indent = [](size_t n) { return Text(n * 3, ' '); };
    const auto object = [&](const Object& values, size_t depth) -> Text {
        if (values.empty()) return "{}";
        Strings entries;
        for (const auto& [name, value] : values)
            entries.push_back(indent(depth + 1) + str(name) + " : " + value);
        return "{\n" + join(entries, ",\n") + "\n" + indent(depth) + "}";
    };
    const auto array = [&](const Strings& values, size_t depth) -> Text {
        if (values.empty()) return "[]";
        Strings entries;
        for (const auto& value : values) entries.push_back(indent(depth + 1) + value);
        return "[\n" + join(entries, ",\n") + "\n" + indent(depth) + "]";
    };
    const auto location = [&](const SourceSpan& span, size_t depth, const Text& generic = {}) {
        Object values{
            {"column", std::to_string(span.begin.column)},
            {"file", str(span.file)},
            {"line", std::to_string(span.begin.line)},
        };
        if (!generic.empty()) {
            values.emplace("kind", str("id"));
            values.emplace("value", str(generic));
        }
        return object(values, depth);
    };
    const auto field_list = [&](const std::vector<Field>& list, size_t depth) {
        Strings out;
        for (const auto& f : list) {
            Object values{
                {"c_name", str(f.c_name)}, {"c_type", str(ftype(f))},
                {"location", location(f.span, depth + 2)}, {"name", str(f.name)},
                {"qualifier", str(f.qualifier ? Text(1, f.qualifier) : Text{})},
                {"type", str(f.type)}, {"unnamed", f.unnamed ? "true" : "false"},
            };
            if (!f.refinement.empty()) values.emplace("refinement", str(f.refinement));
            out.push_back(object(values, depth + 1));
        }
        return array(out, depth);
    };
    Strings defs;
    for (const auto& d : s.definitions) {
        const bool product = d.kind == DefinitionKind::product;
        Object entry{
            {"attributes", field_list(d.attributes, 3)}, {"c_type", str(ctype(d.name))},
            {"kind", str(product ? "product" : "sum")}, {"location", location(d.span, 3)},
            {"name", str(d.name)}, {"representation", str(d.is_enum ? "enum" : "pointer")},
        };
        if (product) {
            entry.emplace("fields", field_list(d.fields, 3));
            entry.emplace("builder", str(p + "make_" + d.name));
        } else {
            Strings ctors;
            for (const auto& c : d.constructors)
                ctors.push_back(object({
                    {"name", str(c.name)}, {"tag", std::to_string(c.tag)},
                    {"c_name", str(p + c.name)}, {"c_tag", str(tag(d, c))},
                    {"fields", field_list(c.fields, 5)}, {"location", location(c.span, 5)},
                }, 4));
            entry.emplace("constructors", array(ctors, 3));
        }
        defs.push_back(object(entry, 2));
    }
    Object generics;
    for (const auto& [name, spec] : s.generics) {
        Strings args;
        for (const auto& arg : spec.args) args.push_back(str(arg));
        generics.emplace(name, object({
            {"args", array(args, 3)}, {"c_type", str(ctype(name))}, {"kind", str(spec.kind)},
            {"location", location(spec.span, 3, spec.kind)},
        }, 2));
    }
    Strings primitives_list;
    for (const auto& name : primitives()) primitives_list.push_back(str(name));
    return object({
        {"format_version", "1"}, {"generator_version", str("2.0")},
        {"generics", object(generics, 1)}, {"guard", str(s.guard)},
        {"module", s.module.empty() ? "null" : str(s.module)}, {"prefix", str(p)},
        {"primitives", array(primitives_list, 1)}, {"types", array(defs, 1)},
    }, 0) + "\n";
}

class CppModel {
public:
    explicit CppModel(const Schema& schema);
    Text emit() const;

private:
    const Schema& s;
    std::map<Text, Text> names;
    std::map<Text, Text> constructors;
    std::set<Text> opaque;
    std::set<Text> used;
    static Text claim(Text name, std::set<Text>& taken);
    Text type(const Text& name) const;
    Text field_type(const Field& field) const;
    struct FieldList {
        Text members;
        Strings descriptors;
    };
    FieldList fields(const std::vector<Field>& list, std::set<Text> taken) const;
};

Text CppModel::claim(Text name, std::set<Text>& taken)
{
    static const std::set<Text> reserved{
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
        "compl", "concept", "const", "const_cast", "consteval", "constexpr", "constinit",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "final", "float", "for", "friend", "goto", "if", "inline", "int",
        "long", "module", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "override", "private", "protected", "public",
        "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof",
        "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "while", "xor", "xor_eq",
        "import", "std", "NULL", "EOF", "SIZE_MAX", "stdin", "stdout", "stderr", "errno",
        "EXIT_SUCCESS", "EXIT_FAILURE", "RAND_MAX", "MB_CUR_MAX", "PTRDIFF_MIN", "PTRDIFF_MAX",
        "SIG_ATOMIC_MIN", "SIG_ATOMIC_MAX", "WCHAR_MIN", "WCHAR_MAX", "WINT_MIN", "WINT_MAX",
    };
    // Imported types may carry a module qualification. Never create names
    // reserved to the implementation (leading underscores or doubled ones).
    name = type_stem(name);
    Text clean;
    for (char c : name)
        if (c != '_' || clean.empty() || clean.back() != '_') clean += c;
    if (clean.empty() || clean.front() == '_') clean.insert(0, "asdl");
    if (reserved.contains(clean) || (clean.starts_with("INT") && (clean.ends_with("_MIN") || clean.ends_with("_MAX")))
        || (clean.starts_with("UINT") && clean.ends_with("_MAX"))) clean += "_";
    const Text base = clean;
    for (size_t i = 2; !taken.insert(clean).second; ++i)
        clean = base + (base.ends_with('_') ? "" : "_") + std::to_string(i);
    return clean;
}

CppModel::CppModel(const Schema& schema) : s(schema)
{
    used.insert("reflection");
    for (const auto& [name, c] : builtin_c_types()) {
        if (name != "array" && name != "list" && name != "map" && name != "buffer" && name != "path") {
            opaque.insert(name);
            used.insert(name + "_handle");
        }
    }
    for (const auto& d : s.definitions) names.emplace(d.name, claim(d.name, used));
    for (const auto& d : s.definitions) {
        std::set<Text> enums{names.at(d.name)};
        for (const auto& c : d.constructors)
            constructors.emplace(c.name, claim(c.name, d.is_enum ? enums : used));
    }
}

Text CppModel::type(const Text& name) const
{
    static const std::map<Text, Text> scalar{
        {"int", "std::intmax_t"}, {"uint", "std::uintmax_t"}, {"bool", "bool"},
        {"real", "long double"}, {"size", "std::ptrdiff_t"}, {"usize", "std::size_t"},
        {"string", "std::string"}, {"identifier", "std::string"}, {"path", "std::string"},
        {"buffer", "std::vector<std::byte>"},
    };
    if (const auto pos = scalar.find(name); pos != scalar.end()) return pos->second;
    if (opaque.contains(name)) return "std::shared_ptr<" + name + "_handle>";
    if (const auto pos = s.generics.find(name); pos != s.generics.end()) {
        const auto& g = pos->second;
        if (g.kind == "list") return "std::vector<" + type(g.args[0]) + ">";
        if (g.kind == "array")
            return "std::array<" + (g.args.size() == 1 ? "std::byte" : type(g.args[0])) + ", " + g.args.back() + ">";
        return "std::map<" + type(g.args[0]) + ", " + type(g.args[1]) + ">";
    }
    const auto& d = s.definitions.at(s.types.at(name));
    const auto& n = names.at(name);
    return d.is_enum ? n : "std::shared_ptr<" + n + ">";
}

Text CppModel::field_type(const Field& f) const
{
    auto t = type(f.type);
    if (f.qualifier == '?') return "std::optional<" + t + ">";
    if (f.qualifier == '*' || f.qualifier == '+') return "std::vector<" + t + ">";
    return t;
}

CppModel::FieldList CppModel::fields(const std::vector<Field>& list, std::set<Text> taken) const
{
    FieldList out;
    // Unqualified type names must not be shadowed by any data member, since
    // those names also appear in subsequent field declarations.
    taken.insert(used.begin(), used.end());
    for (const auto& f : list) {
        const auto name = claim(f.name, taken), t = field_type(f);
        out.members += "    " + t + " " + name + "{};\n";
        const Text cardinality = f.qualifier == '?' ? "optional" : f.qualifier == '*' ? "sequence"
            : f.qualifier == '+' ? "nonempty_sequence" : "one";
        out.descriptors.push_back("{ " + quote(f.name) + ", " + quote(name) + ", " + quote(f.c_name)
            + ", " + quote(f.type) + ", " + quote(t) + ", " + quote(asdl::field_type(s, f))
            + ", cardinality::" + cardinality + ", " + (f.unnamed ? "true" : "false") + ", " + quote(f.refinement) + " }");
    }
    return out;
}

Text CppModel::emit() const
{
    std::set<Text> namespace_names;
    const auto ns = claim(s.prefix + "ast", namespace_names);
    const auto guard = s.guard + "_CPP20";
    Text out = "/* Generated by asdl. Standalone C++20 value and graph types. */\n#ifndef "
        + guard + "\n#define " + guard + R"ASDL(

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

)ASDL" "namespace " + ns + " {\n\n";
    for (const auto& name : opaque) out += "struct " + name + "_handle;\n";
    for (const auto& d : s.definitions)
        out += (d.is_enum ? "enum class " + names.at(d.name) + " : std::size_t" : "struct " + names.at(d.name)) + ";\n";
    out += "\n";

    Text metadata;
    const auto array = [&](View type_name, const Text& name, const Strings& values) {
        metadata += "inline constexpr std::array<" + Text(type_name) + ", "
            + std::to_string(values.size()) + "> " + name;
        if (values.empty()) {
            metadata += "{};\n";
        } else {
            metadata += "{{\n";
            for (const auto& value : values) metadata += "    " + value + ",\n";
            metadata += "}};\n";
        }
    };
    Strings type_descriptors;
    size_t index = 0;
    for (const auto& d : s.definitions) {
        const auto id = std::to_string(index++);
        const auto& n = names.at(d.name);
        const auto variant_type = n == "variant_type" ? "variant_type_" : "variant_type";
        const auto value = n == "value" ? "value_" : "value";
        const auto attrs = fields(d.attributes, {value, variant_type});
        const auto payload = fields(d.fields, {});
        array("field", "attributes_" + id, attrs.descriptors);
        array("field", "fields_" + id, payload.descriptors);
        Strings variants, ctors;
        if (d.is_enum) out += "enum class " + n + " : std::size_t {\n";
        for (const auto& c : d.constructors) {
            const auto& cn = constructors.at(c.name);
            const auto tag = std::to_string(c.tag), fd = "fields_" + id + "_" + tag;
            const auto cfields = fields(c.fields, {});
            array("field", fd, cfields.descriptors);
            const auto cpp_name = ns + "::" + (d.is_enum ? n + "::" : "") + cn;
            ctors.push_back("{ " + quote(c.name) + ", " + quote(cpp_name) + ", "
                + quote(s.prefix + c.name) + ", " + quote(tag_name(s, d, c)) + ", " + tag + ", " + fd + " }");
            if (d.is_enum) {
                out += "    " + cn + " = " + tag + ",\n";
            } else {
                out += "struct " + cn + " {\n" + cfields.members + "};\n";
                variants.push_back(cn);
            }
        }
        if (d.is_enum) {
            out += "};\n";
        } else {
            out += "struct " + n + " {\n";
            if (d.kind == DefinitionKind::product)
                out += payload.members;
            else {
                // Use fully qualified constructor names to avoid shadowing by
                // variant_type or value, independently of schema spelling.
                for (auto& variant : variants) variant = "::" + ns + "::" + variant;
                out += "    using " + Text(variant_type) + " = std::variant<" + join(variants, ", ") + ">;\n";
                out += "    " + Text(variant_type) + " " + value + "{};\n" + attrs.members;
            }
            out += "};\n";
        }
        out += "\n";
        array("constructor", "constructors_" + id, ctors);
        const auto kind = d.is_enum ? "enumeration" : d.kind == DefinitionKind::product ? "product" : "sum";
        type_descriptors.push_back("{ " + quote(d.name) + ", " + quote(ns + "::" + n)
            + ", " + quote(c_type(s, d.name)) + ", kind::" + kind + ", fields_" + id + ", attributes_" + id
            + ", constructors_" + id + " }");
    }
    array("type", "types", type_descriptors);
    out += R"ASDL(namespace reflection {

enum class cardinality { one, optional, sequence, nonempty_sequence };
enum class kind { product, sum, enumeration };

struct field {
    std::string_view name, cpp_name, c_name, asdl_type, cpp_type, c_type;
    cardinality qualifier;
    bool unnamed;
    std::string_view refinement;
};
struct constructor {
    std::string_view name, cpp_name, c_name, c_tag;
    std::size_t tag;
    std::span<const field> fields;
};
struct type {
    std::string_view name, cpp_name, c_type;
    kind representation;
    std::span<const field> fields, attributes;
    std::span<const constructor> constructors;
};

)ASDL";
    out += "inline constexpr std::string_view module = " + quote(s.module) + ";\n\n" + metadata
        + "\n} // namespace reflection\n} // namespace " + ns + "\n\n#endif /* " + guard + " */\n";
    return out;
}

Text Emitter::cpp20() const
{
    return CppModel(s).emit();
}

GeneratedOutputs Emitter::emit()
{
    macro_symbol(s.guard, "header guard");
    Text types = runtime_types();
    types += ast_types();
    types += inventory();
    runtime_functions();
    generic_functions();
    collection_functions();
    builders();
    validators();
    const bool header_only = enabled("header-only");
    const Text linkage = header_only ? "static inline " : "";
    Text prototypes, bodies;
    for (const auto& [signature, body] : functions) {
        prototypes += linkage + signature + ";\n";
        if (!bodies.empty()) bodies += '\n';
        bodies += linkage + signature + "\n{\n" + body + "}\n";
    }

    GeneratedOutputs out;
    auto runtime = builtins();
    const auto header = opt("header");
    const auto header_label = opt("header", "<asdl-header>");
    if (const auto path = opt("builtins-header"); !path.empty()) {
        out.builtins_header = std::move(runtime);
        runtime = include_line(relative_include(path, header.empty() ? "asdl.h" : header));
    }
    out.header = "/* Generated by asdl. C11; edit the ASDL input. */\n"
        "#ifndef " + s.guard + "\n#define " + s.guard + "\n\n"
        "#include <stddef.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <stdbool.h>\n#include <sys/types.h>\n"
        + injected(s.prologue, header_label) + runtime + "\n" + types + "/* ---- API ---- */\n" + prototypes + "\n";
    if (header_only) {
        out.header += "/* ---- implementation ---- */\n" + bodies + injected(s.epilogue, header_label);
    } else {
        const auto source = opt("source");
        out.source = "/* Generated by asdl. */\n" + header_include(source)
            + "\n" + bodies + injected(s.epilogue, source);
    }
    out.header += "\n#endif /* " + s.guard + " */\n";
    if (const auto path = opt("visitor"); !path.empty()) out.visitor = visitor(path);
    if (const auto path = opt("listener"); !path.empty())
        out.listener = header_include(path) + apply_template(rt::listener);
    if (!opt("schema").empty()) out.schema = schema_json();
    if (!opt("cpp-header").empty()) out.cpp_header = cpp20();
    return out;
}

} // namespace

GeneratedOutputs generate(const Schema& schema)
{
    return Emitter(schema).emit();
}

} // namespace asdl
