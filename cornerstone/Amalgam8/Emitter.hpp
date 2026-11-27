#pragma once

#include "Contracts.hpp"

#include <sstream>

namespace am8 {
namespace emitter_detail {

// Build the declarator from the identifier outward. A pointer must bind less
// tightly than the suffix of its pointee when that pointee is an array/function.
inline std::string declarator(const Type& type, std::string name) {
    switch (type.kind) {
    case TypeKind::pointer: {
        std::string prefix = "*";
        if (type.is_const) prefix += " const";
        if (type.is_volatile) prefix += " volatile";
        if (prefix.size() > 1 && !name.empty()) prefix += ' ';
        name = prefix + name;
        const auto& pointee = type.children.at(0);
        if (pointee.kind == TypeKind::array || pointee.kind == TypeKind::function)
            name = '(' + name + ')';
        return declarator(pointee, std::move(name));
    }
    case TypeKind::array: {
        name += '[' + std::to_string(type.extent) + ']';
        if (type.is_const || type.is_volatile) {
            auto element = type.children.at(0);
            element.is_const |= type.is_const;
            element.is_volatile |= type.is_volatile;
            return declarator(element, std::move(name));
        }
        return declarator(type.children.at(0), std::move(name));
    }
    case TypeKind::function:
        name += '(';
        if (type.children.size() == 1) name += "void";
        for (std::size_t i = 1; i < type.children.size(); ++i) {
            if (i > 1) name += ", ";
            name += declarator(type.children[i], {});
        }
        name += ')';
        return declarator(type.children.at(0), std::move(name));
    case TypeKind::named:
    case TypeKind::opaque: {
        std::string quals;
        if (type.is_const) quals += "const ";
        if (type.is_volatile) quals += "volatile ";
        // Legacy text is restricted to simple type specifiers and pointers.
        // A qualifier on such a pointer belongs after its last '*'.
        if (!quals.empty() && type.kind == TypeKind::opaque &&
            type.name.find('*') != std::string::npos) {
            quals.pop_back();
            return type.name + ' ' + quals + (name.empty() ? "" : ' ' + name);
        }
        return quals + type.name + (name.empty() ? "" : ' ' + name);
    }
    }
    fail(type.span, "unhandled type in emitter");
}

inline std::string function_signature(const Decl& decl) {
    std::string name = decl.name + '(';
    if (decl.parameters.empty()) name += "void";
    for (std::size_t i = 0; i < decl.parameters.size(); ++i) {
        if (i) name += ", ";
        const auto& parameter = decl.parameters[i];
        name += declarator(parameter.type, parameter.name);
    }
    name += ')';
    return declarator(decl.type, std::move(name));
}

inline std::string string_literal(std::string_view text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c >= 32 && c <= 126 && c != '?') {
            result += static_cast<char>(c);
        } else {
            // Exactly three digits prevent adjoining source digits from
            // extending the escape, and escaping '?' prevents C11 trigraphs.
            result += '\\';
            result += static_cast<char>('0' + (c >> 6));
            result += static_cast<char>('0' + ((c >> 3) & 7));
            result += static_cast<char>('0' + (c & 7));
        }
    }
    return result + '"';
}

inline std::string expression(const Expr& expr) {
    const auto child = [&](std::size_t i) { return expression(expr.children.at(i)); };
    switch (expr.kind) {
    case ExprKind::name:
    case ExprKind::number:
        return expr.text;
    case ExprKind::opaque:
        // Preserve the boundary of a raw expression inside structured
        // operators and calls, e.g. a comma expression is one argument.
        return '(' + expr.text + ')';
    case ExprKind::string:
        return string_literal(expr.text);
    case ExprKind::call: {
        std::string result = expr.text + '(';
        for (std::size_t i = 0; i < expr.children.size(); ++i) {
            if (i) result += ", ";
            result += child(i);
        }
        return result + ')';
    }
    case ExprKind::unary:
        return '(' + expr.text + '(' + child(0) + "))";
    case ExprKind::binary:
        return '(' + child(0) + ' ' + expr.text + ' ' + child(1) + ')';
    case ExprKind::field:
        return "((" + child(0) + ")." + expr.text + ')';
    case ExprKind::ptr_field:
        return "((" + child(0) + ")->" + expr.text + ')';
    case ExprKind::index:
        return "((" + child(0) + ")[" + child(1) + "])";
    }
    fail(expr.span, "unhandled expression in emitter");
}

inline std::string initializer(const Expr& expr) {
    // A raw initializer can explicitly supply a C brace-list rather than an
    // expression; parentheses would turn it into invalid source.
    const auto start = expr.text.find_first_not_of(" \t\r\n\v\f");
    if (expr.kind == ExprKind::opaque && start != std::string::npos && expr.text[start] == '{')
        return expr.text;
    return expression(expr);
}

inline void raw_source(std::ostream& out, std::string_view raw) {
    out << raw;
    if (raw.empty() || raw.back() != '\n') out << '\n';
}

inline void statement(std::ostream& out, const Stmt& stmt, std::size_t indent);

inline void block(std::ostream& out, const Stmt& stmt, std::size_t indent) {
    out << "{\n";
    if (stmt.kind == StmtKind::block) {
        for (const auto& child : stmt.children) statement(out, child, indent + 4);
    } else {
        statement(out, stmt, indent + 4);
    }
    out << std::string(indent, ' ') << '}';
}

inline void statement(std::ostream& out, const Stmt& stmt, std::size_t indent) {
    const std::string pad(indent, ' ');
    const auto expr = [&](std::size_t i) { return expression(stmt.expressions.at(i)); };
    switch (stmt.kind) {
    case StmtKind::block:
        out << pad;
        block(out, stmt, indent);
        out << '\n';
        break;
    case StmtKind::local:
        out << pad << declarator(stmt.type, stmt.text);
        if (!stmt.expressions.empty()) out << " = " << initializer(stmt.expressions.front());
        out << ";\n";
        break;
    case StmtKind::assign:
        out << pad << expr(0) << " = " << expr(1) << ";\n";
        break;
    case StmtKind::branch:
        out << pad << "if (" << expr(0) << ") ";
        block(out, stmt.children.at(0), indent);
        if (stmt.children.size() > 1) {
            out << " else ";
            block(out, stmt.children[1], indent);
        }
        out << '\n';
        break;
    case StmtKind::loop:
        out << pad << "while (" << expr(0) << ") ";
        block(out, stmt.children.at(0), indent);
        out << '\n';
        break;
    case StmtKind::return_:
        out << pad << "return";
        if (!stmt.expressions.empty()) out << ' ' << expr(0);
        out << ";\n";
        break;
    case StmtKind::expression:
        out << pad << expr(0) << ";\n";
        break;
    case StmtKind::opaque:
        raw_source(out, stmt.text);
        break;
    }
}

inline void dependency(std::ostream& out, const Dependency& dep) {
    if (dep.context == Context::compiler) return;
    if (dep.context == Context::cpp) out << "#ifdef __cplusplus\n";
    if (dep.kind == DependencyKind::bundled)
        raw_source(out, dep.bundled_source);
    else if (!dep.header.empty())
        out << "#include <" << dep.header << ">\n";
    if (!dep.adapter_source.empty()) raw_source(out, dep.adapter_source);
    if (dep.context == Context::cpp) out << "#endif\n";
}

inline void declaration(std::ostream& out, const Decl& decl, const std::string& unused) {
    switch (decl.kind) {
    case DeclKind::constant:
        out << "#define " << decl.name << " (" << expression(decl.initializer.value()) << ")\n";
        break;
    case DeclKind::alias:
        out << "typedef " << declarator(decl.type, decl.name) << ";\n";
        break;
    case DeclKind::record:
        out << "struct " << decl.name << " {\n";
        for (const auto& field : decl.parameters)
            out << "    " << declarator(field.type, field.name) << ";\n";
        out << "};\n";
        break;
    case DeclKind::enum_:
        out << "typedef enum " << decl.name << " {\n";
        for (const auto& [name, value] : decl.enumerators) {
            out << "    " << name;
            if (value) out << " = " << expression(*value);
            out << ",\n";
        }
        out << "} " << decl.name << ";\n";
        break;
    case DeclKind::function:
        out << unused << " static inline " << function_signature(decl);
        if (decl.body) {
            out << ' ';
            block(out, *decl.body, 0);
            out << '\n';
        } else {
            out << ";\n";
        }
        break;
    case DeclKind::variable:
        out << unused << " static " << declarator(decl.type, decl.name);
        if (decl.initializer) out << " = " << initializer(*decl.initializer);
        out << ";\n";
        break;
    case DeclKind::external:
        out << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
            << "extern " << declarator(decl.type, decl.name) << ";\n"
            << "#ifdef __cplusplus\n}\n#endif\n";
        break;
    case DeclKind::raw_c:
        raw_source(out, decl.raw);
        break;
    case DeclKind::raw_cpp:
        out << "#ifdef __cplusplus\n";
        raw_source(out, decl.raw);
        out << "#endif\n";
        break;
    }
}

} // namespace emitter_detail

inline std::string emit(const Unit& unit, Mode mode) {
    if (mode == Mode::cli && !unit.entry)
        fail(unit.span, "--cli requires (cli ENTRY) or (cli \"body\")");

    std::ostringstream out;
    const auto guard = "AMALGAM8_" + unit.name + "_HPP";
    out << "#ifndef " << guard << "\n#define " << guard << "\n";
    for (const auto& dep : unit.dependencies) emitter_detail::dependency(out, dep);
    // Record names are usable from any declaration; lowering separately
    // schedules complete definitions where a by-value use requires one.
    for (const auto& decl : unit.declarations)
        if (decl.kind == DeclKind::record)
            out << "typedef struct " << decl.name << ' ' << decl.name << ";\n";
    const auto unused = "AMALGAM8_" + unit.name + "_UNUSED";
    const bool has_storage = std::any_of(unit.declarations.begin(), unit.declarations.end(),
        [](const Decl& decl) {
            return decl.kind == DeclKind::function || decl.kind == DeclKind::variable;
        });
    if (has_storage) {
        out << "#if (defined(__cplusplus) && __cplusplus >= 201703L) || "
            << "(defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)\n"
            << "#define " << unused << " [[maybe_unused]]\n"
            << "#elif defined(__GNUC__) || defined(__clang__)\n"
            << "#define " << unused << " __attribute__((__unused__))\n"
            << "#else\n#define " << unused << "\n#endif\n";
    }
    for (const auto& decl : unit.declarations) emitter_detail::declaration(out, decl, unused);
    if (has_storage) out << "#undef " << unused << '\n';
    out << "#endif /* " << guard << " */\n";
    if (mode == Mode::cli) {
        const auto& entry = *unit.entry;
        const auto cli_guard = "AMALGAM8_" + unit.name + "_CLI";
        out << "#ifndef " << cli_guard << "\n#define " << cli_guard << "\n";
        out << "int main(int argc, char **argv) {\n";
        if (entry.raw) {
            out << "    (void)argc;\n    (void)argv;\n";
            emitter_detail::raw_source(out, entry.text);
        } else {
            out << "    return " << entry.text << "(argc, argv);\n";
        }
        out << "}\n#endif /* " << cli_guard << " */\n";
    }
    return out.str();
}

} // namespace am8
