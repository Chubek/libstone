#include "asdl/dot.hpp"
#include "asdl/schema.hpp"

#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace asdl {
namespace {

std::string quote(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break; // Graphviz expands escapes such as \N.
        case '\n': result += "\\n"; break;
        case '\r': result += "\\\\r"; break;
        case '\t': result += "\\\\t"; break;
        default:
            if (ch < 0x20 || ch == 0x7f) {
                result += "\\\\x";
                result += hex[ch >> 4];
                result += hex[ch & 15];
            } else {
                result += static_cast<char>(ch);
            }
        }
    }
    result += '"';
    return result;
}

std::string module_label(const Schema &schema)
{
    if (!schema.module.empty()) return schema.module;
    if (!schema.language.empty()) return schema.language;
    return "anonymous schema";
}

std::string type_node(std::size_t i) { return "type_" + std::to_string(i); }

void node(std::ostream &out, std::string_view id, std::string_view shape,
          std::string_view label)
{
    out << "  " << id << " [shape=" << shape << ", label=" << quote(label) << "];\n";
}

void edge(std::ostream &out, std::string_view from, std::string_view to,
          std::string_view label = {}, bool dashed = false)
{
    out << "  " << from << " -> " << to;
    if (!label.empty() && dashed) out << " [label=" << quote(label) << ", style=dashed]";
    else if (!label.empty()) out << " [label=" << quote(label) << ']';
    else if (dashed) out << " [style=dashed]";
    out << ";\n";
}

std::string field_signature(const Field &field)
{
    std::string result = field.type;
    if (!field.refinement.empty()) result += '<' + field.refinement + '>';
    if (field.qualifier) result += field.qualifier;
    if (!field.name.empty() && !field.unnamed) result += ' ' + field.name;
    return result;
}

void fields(std::ostream &out, const Schema &schema, DotView view,
            const std::string &owner, const std::string &type,
            const std::vector<Field> &list, const std::string &role)
{
    for (std::size_t i = 0; i < list.size(); ++i) {
        const auto &field = list[i];
        const auto signature = field_signature(field);
        const auto id = owner + "_field_" + std::to_string(i);
        if (view == DotView::schema) {
            node(out, id, "plaintext", signature);
            edge(out, owner, id);
        }
        // A generic field can reference several types. Report each dependency
        // once per field; omit builtins and other non-definition names.
        const auto emit = [&](const std::string &target) {
            const auto found = schema.types.find(target);
            if (found == schema.types.end()) return;
            if (view == DotView::schema)
                edge(out, id, type_node(found->second), {}, true);
            else
                edge(out, type, type_node(found->second), role + signature);
        };
        if (schema.types.count(field.type)) {
            emit(field.type);
        } else if (schema.generics.count(field.type)) {
            std::set<std::string> seen;
            for (const auto &target : type_dependencies(schema, field.type)) {
                if (seen.insert(target).second) emit(target);
            }
        }
    }
}

void types(std::ostream &out, const Schema &schema, DotView view)
{
    for (std::size_t i = 0; i < schema.definitions.size(); ++i) {
        const auto &definition = schema.definitions[i];
        const auto id = type_node(i);
        const char *kind = definition.kind == DefinitionKind::product ? "product" : "sum";
        node(out, id, "box", definition.name + " (" + kind + ')');
        if (view == DotView::schema) edge(out, "root", id);
        fields(out, schema, view, id, id, definition.fields, "");

        for (std::size_t j = 0; j < definition.constructors.size(); ++j) {
            const auto &ctor = definition.constructors[j];
            const auto ctor_id = id + "_ctor_" + std::to_string(j);
            if (view == DotView::schema) {
                node(out, ctor_id, "ellipse", ctor.name);
                edge(out, id, ctor_id);
            }
            fields(out, schema, view, ctor_id, id, ctor.fields, ctor.name + ": ");
        }
        if (!definition.attributes.empty()) {
            const auto attr_id = id + "_attributes";
            if (view == DotView::schema) {
                node(out, attr_id, "ellipse", "attributes");
                edge(out, id, attr_id);
            }
            fields(out, schema, view, attr_id, id, definition.attributes, "attributes: ");
        }
    }
}

void imports(std::ostream &out, const Schema &schema)
{
    // Deduplicate destination nodes, not statements: independent imports of
    // the same module can bring different names into scope.
    std::map<std::pair<bool, std::string>, std::string> modules;
    for (const auto &import : schema.imports) {
        const auto id = "import_" + std::to_string(modules.size());
        const auto [pos, inserted] = modules.emplace(std::make_pair(import.schema, import.module), id);
        if (inserted)
            node(out, id, "component", import.module + (import.schema ? " (schema)" : " (Lua)"));
        std::string bindings;
        if (!import.alias.empty()) bindings = "as " + import.alias;
        for (const auto &name : import.names) {
            if (!bindings.empty()) bindings += ", ";
            bindings += name.name;
            if (!name.alias.empty()) bindings += " as " + name.alias;
        }
        edge(out, "root", pos->second, bindings);
    }
}

} // namespace

std::string emit_dot(const Schema &schema, DotView view)
{
    std::ostringstream out;
    out << "digraph ASDL {\n  rankdir=LR;\n";
    switch (view) {
    case DotView::schema:
        node(out, "root", "folder", module_label(schema));
        types(out, schema, view);
        imports(out, schema);
        break;
    case DotView::dependencies:
        out << "  label=" << quote(module_label(schema)) << ";\n";
        types(out, schema, view);
        break;
    case DotView::imports:
        node(out, "root", "folder", module_label(schema));
        imports(out, schema);
        break;
    default:
        throw std::invalid_argument("invalid DOT view");
    }
    out << "}\n";
    return out.str();
}

} // namespace asdl
