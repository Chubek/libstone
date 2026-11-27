#pragma once

#include "IR.hpp"

#include <charconv>

namespace am8::semantic {

inline const std::set<std::string_view> reserved = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "const_cast", "consteval", "constexpr",
    "constinit", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if", "import",
    "inline", "int", "long", "module", "mutable", "namespace", "new", "noexcept",
    "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "restrict", "return",
    "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
    "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor", "xor_eq", "_Alignas", "_Alignof",
    "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary", "_Noreturn",
    "_Static_assert", "_Thread_local"
};

inline const std::set<std::string_view> integers = {
    "char", "signed char", "unsigned char", "short", "unsigned short", "int",
    "unsigned int", "long", "unsigned long", "long long", "unsigned long long",
    "bool", "size_t", "ptrdiff_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
    "int32_t", "uint32_t", "int64_t", "uint64_t", "intptr_t", "uintptr_t",
    "intmax_t", "uintmax_t"
};

inline std::string c_identifier(const Syntax& node) {
    auto name = identifier(node);
    if (reserved.contains(name)) fail(node, "reserved C/C++ identifier '" + name + "'");
    return name;
}

inline std::string reference(const Syntax& node) {
    const auto name = atom(node);
    const auto dot = name.find('.');
    const auto first = std::string_view(name).substr(0, dot);
    if (!node.quoted && is_identifier(first) &&
        (dot == std::string::npos || is_identifier(std::string_view(name).substr(dot + 1))))
        return name;
    fail(node, "expected a name, got '" + name + "'");
}

inline void arity(const Syntax& node, std::size_t min, std::size_t max) {
    const auto n = node.items.empty() ? 0 : node.items.size() - 1;
    if (node.is_atom || n < min || n > max)
        fail(node, "'" + std::string(head(node)) + "' expects " + std::to_string(min) +
                   (min == max ? "" : " to " + std::to_string(max)) + " operand(s)");
}

inline std::string canonical_base(std::string_view name) {
    if (name == "signed" || name == "signed int") return "int";
    if (name == "unsigned") return "unsigned int";
    if (name == "short int" || name == "signed short" || name == "signed short int") return "short";
    if (name == "long int" || name == "signed long" || name == "signed long int") return "long";
    if (name == "long long int" || name == "signed long long" || name == "signed long long int")
        return "long long";
    if (name == "unsigned short int") return "unsigned short";
    if (name == "unsigned long int") return "unsigned long";
    if (name == "unsigned long long int") return "unsigned long long";
    if (name == "_Bool") return "bool";
    return std::string(name);
}

inline bool builtin(std::string_view name) {
    return name == "void" || integers.contains(name) || name == "float" ||
           name == "double" || name == "long double";
}

inline std::optional<std::string> numeric_type(std::string_view text) {
    std::size_t p = 0;
    const auto has = [&](char c) { return p < text.size() && text[p] == c; };
    if (has('+') || has('-')) ++p;
    const auto start = p;
    bool hex = p + 1 < text.size() && has('0') && (text[p+1] == 'x' || text[p+1] == 'X');
    if (hex) p += 2;
    const auto digits = [&] {
        const auto before = p;
        while (p < text.size() && (digit(text[p]) ||
               (hex && ((text[p] >= 'a' && text[p] <= 'f') || (text[p] >= 'A' && text[p] <= 'F')))))
            ++p;
        return p - before;
    };
    const auto leading = digits();
    bool point = has('.');
    std::size_t fractional = 0;
    if (point) { ++p; fractional = digits(); }
    if (!leading && !fractional) return {};
    const bool exponent = hex ? (has('p') || has('P')) : (has('e') || has('E'));
    if (exponent) {
        ++p;
        if (has('+') || has('-')) ++p;
        const auto before = p;
        while (p < text.size() && digit(text[p])) ++p;
        if (p == before) return {};
    }
    const auto suffix = text.substr(p);
    if (point || exponent) {
        if (hex && !exponent) return {};
        if (suffix.empty()) return "double";
        if (suffix == "f" || suffix == "F") return "float";
        if (suffix == "l" || suffix == "L") return "long double";
        return {};
    }
    if (!hex && text[start] == '0' &&
        text.substr(start, leading).find_first_of("89") != std::string_view::npos)
        return {};
    std::string normalized;
    for (char c : suffix) normalized += c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    if (suffix.find("lL") != std::string_view::npos || suffix.find("Ll") != std::string_view::npos)
        return {};
    if (normalized.empty()) return "int";
    if (normalized == "u") return "unsigned int";
    if (normalized == "l") return "long";
    if (normalized == "ul" || normalized == "lu") return "unsigned long";
    if (normalized == "ll") return "long long";
    if (normalized == "ull" || normalized == "llu") return "unsigned long long";
    return {};
}

struct Value {
    Expr expr;
    Type type;
    bool lvalue{};
    bool constant{};
    bool null{};
    bool static_address{};
};

class Lowerer {
    enum class State { fresh, busy, done };
    struct Symbol {
        std::size_t index;
        std::string emitted;
        std::optional<std::size_t> enumerator;
        std::size_t origin;
    };
    struct Work {
        const Syntax* node;
        std::size_t module;
        Decl decl;
        std::set<std::size_t> dependencies;
        std::set<std::size_t> prototypes;
        std::set<std::size_t> signature_dependencies;
        State signature{State::fresh};
        State initializer{State::fresh};
        std::optional<Value> value;
        std::vector<const Syntax*> enum_values;
    };

    const Program& program;
    Unit unit;
    std::vector<Work> work;
    std::vector<std::map<std::string, Symbol, std::less<>>> names;
    std::map<std::string, Symbol, std::less<>> emitted;
    std::map<std::string, std::size_t, std::less<>> public_counts;
    std::set<std::string, std::less<>> external_names;
    std::map<std::string, std::string, std::less<>> c_names;
    std::vector<std::set<std::string, std::less<>>> cpp_types;
    const Syntax* entry{};
    std::size_t signature_depth{}, initializer_depth{}, canonical_depth{}, member_depth{};
    std::size_t canonical_steps{};
    std::size_t local_number_{};

    struct Local {
        Type type;
        std::string emitted;
    };
    struct Scope {
        std::map<std::pair<std::size_t, std::string>, Local> bindings;
        std::set<std::string, std::less<>> emitted_names;
    };
    struct Context {
        std::size_t module;
        std::size_t owner;
        const Type* result{};
        std::vector<Scope> scopes;
        std::optional<std::size_t> enumerator;
    };

    struct Depth {
        std::size_t& depth;
        Depth(std::size_t& n, const Syntax& at) : depth(n) {
            if (++depth > 256) fail(at, "semantic dependency nesting limit exceeded");
        }
        ~Depth() { --depth; }
    };

    bool visible(std::size_t module, std::string_view name) const {
        return program.modules[module].exports.contains(std::string(name));
    }

    std::size_t origin(std::size_t module, const Syntax& at) const {
        const auto result = at.origin_module.value_or(module);
        if (result >= names.size()) fail(at, "invalid expansion module");
        return result;
    }

    const Symbol* lookup(std::size_t module, const Syntax& at, const std::string& name) const {
        const auto receiver = module;
        module = origin(module, at);
        const auto& m = program.modules[module];
        const auto dot = name.find('.');
        const Symbol* result = nullptr;
        const auto find = [&](std::size_t target, std::string_view key) {
            if (auto it = names[target].find(key); it != names[target].end()) {
                if (result && result != &it->second)
                    fail(at, "ambiguous name '" + name + "'");
                result = &it->second;
            }
        };
        if (dot == std::string::npos) {
            find(module, name);
            for (const auto& import : m.imports)
                if (import.alias.empty() && visible(import.target, name)) find(import.target, name);
            if (!result && receiver != module) {
                if (const auto it = names[receiver].find(name);
                    it != names[receiver].end() && it->second.origin == module)
                    result = &it->second;
            }
        } else {
            const auto key = name.substr(dot + 1);
            for (const auto& import : m.imports)
                if (import.alias == name.substr(0, dot) && visible(import.target, key))
                    find(import.target, key);
        }
        return result;
    }

    std::string claim(const Syntax& at, std::size_t module, std::size_t i,
                      std::optional<std::size_t> enumerator = {}, bool external = false) {
        const auto name = c_identifier(at);
        if (name == "main" || name == "AMALGAM8_" + unit.name + "_HPP" ||
            name == "AMALGAM8_" + unit.name + "_CLI" || name == "AMALGAM8_" + unit.name + "_UNUSED" ||
            generated_name(name))
            fail(at, "reserved symbol '" + name + "'");
        if (cpp_types[module].contains(name)) fail(at, "duplicate symbol '" + name + "'");
        const bool public_name = module == program.root || visible(module, name);
        const bool keep_name = external || module == program.root ||
                               (public_name && !external_names.contains(name) && public_counts.at(name) == 1);
        std::string unique = keep_name ? name : "am8_m" + std::to_string(module) + "_" + name;
        Symbol sym{i, unique, enumerator, origin(module, at)};
        if (!names[module].emplace(name, sym).second)
            fail(at, "duplicate symbol '" + name + "'");
        if (!emitted.emplace(unique, sym).second) fail(at, "conflicting C linkage name '" + name + "'");
        c_names.emplace(unique, name);
        return unique;
    }

    static bool generated_name(std::string_view name) {
        return (name.starts_with("am8_m") && name.size() > 5 && digit(name[5])) ||
               name.starts_with("am8_local_");
    }

    std::string local_name(const Syntax& at) {
        auto name = c_identifier(at);
        if (generated_name(name)) fail(at, "reserved local name '" + name + "'");
        return name;
    }

    static const Syntax& enumerator_name(const Syntax& item) {
        if (item.is_atom) return item;
        if (item.items.size() != 2) fail(item, "expected NAME or (NAME VALUE) enumerator");
        return item.items.front();
    }

    void collect() {
        for (std::size_t m = 0; m < names.size(); ++m) {
            const auto count = [&](const Syntax& node) {
                if (node.is_atom && (m == program.root || visible(m, node.text)))
                    ++public_counts[node.text];
            };
            for (const auto& node : program.modules[m].forms) {
                const auto op = head(node);
                const auto& a = node.items;
                if (a.size() > 1 && (op == "def" || op == "type" || op == "record" || op == "enum"))
                    count(a[1]);
                else if (a.size() > 2 && (op == "fn" || op == "var" || op == "extern"))
                    count(a[2]);
                if (a.size() > 2 && op == "extern" && a[2].is_atom)
                    external_names.insert(a[2].text);
                if (op == "enum" && a.size() == 3 && !a[2].is_atom)
                    for (const auto& e : a[2].items) count(enumerator_name(e));
            }
        }
        std::optional<std::size_t> last_raw;
        for (std::size_t m = 0; m < program.modules.size(); ++m) {
            for (const auto& node : program.modules[m].forms) {
                const auto op = head(node);
                const auto& a = node.items;
                if (op == "include" || op == "use") continue;
                if (op == "cli") {
                    arity(node, 1, 1);
                    if (m != program.root) continue;
                    if (entry) fail(node, "duplicate cli declaration");
                    entry = &a[1];
                    continue;
                }
                if (op == "cpp-type") {
                    arity(node, 1, 1);
                    const auto name = c_identifier(a[1]);
                    if (names[m].contains(name) || !cpp_types[m].insert(name).second)
                        fail(a[1], "duplicate symbol '" + name + "'");
                    continue;
                }
                DeclKind kind;
                std::size_t name_index = 1;
                if (op == "def") { arity(node, 2, 2); kind = DeclKind::constant; }
                else if (op == "type") { arity(node, 2, 2); kind = DeclKind::alias; }
                else if (op == "record") { arity(node, 2, 2); kind = DeclKind::record; }
                else if (op == "enum") { arity(node, 2, 2); kind = DeclKind::enum_; }
                else if (op == "fn") { arity(node, 4, 4); kind = DeclKind::function; name_index = 2; }
                else if (op == "extern") { arity(node, 3, 3); kind = DeclKind::external; name_index = 2; }
                else if (op == "var") { arity(node, 3, 3); kind = DeclKind::variable; name_index = 2; }
                else if (op == "c" || op == "cpp") {
                    arity(node, 1, 1);
                    kind = op == "c" ? DeclKind::raw_c : DeclKind::raw_cpp;
                } else fail(node, "unexpected declaration after expansion '" + std::string(op) + "'");

                const auto i = work.size();
                Decl decl;
                decl.kind = kind;
                decl.span = node.span;
                Work item{&node, m, std::move(decl), {}, {}, {}, State::fresh, State::fresh, {}, {}};
                if (kind == DeclKind::raw_c || kind == DeclKind::raw_cpp) {
                    item.decl.raw = quoted_text(a[1]);
                    if (last_raw) item.dependencies.insert(*last_raw);
                    last_raw = i;
                } else {
                    item.decl.name = claim(a[name_index], m, i, {}, kind == DeclKind::external);
                    item.decl.exported = m == program.root || visible(m, a[name_index].text);
                    // Quoted C declarations can define opaque types and preprocessor
                    // state. Do not hoist a declaration ahead of preceding raw source.
                    if (last_raw) item.dependencies.insert(*last_raw);
                }
                if (kind == DeclKind::enum_) {
                    if (a[2].is_atom || a[2].items.empty())
                        fail(a[2], "expected a nonempty list of enumerators");
                    for (const auto& e : a[2].items) {
                        auto name = claim(enumerator_name(e), m, i, item.enum_values.size());
                        item.enum_values.push_back(e.is_atom ? nullptr : &e.items[1]);
                        item.decl.enumerators.emplace_back(std::move(name), std::nullopt);
                    }
                }
                work.push_back(std::move(item));
            }
        }
    }

    static bool type_decl(DeclKind kind) {
        return kind == DeclKind::alias || kind == DeclKind::record || kind == DeclKind::enum_;
    }

    void require_system_header(std::string_view header, const Span& span) {
        for (const auto& d : unit.dependencies)
            if (d.header == header && d.context == am8::Context::c) return;
        Dependency dependency;
        dependency.name = dependency.header = header;
        dependency.context = am8::Context::c;
        dependency.kind = DependencyKind::system;
        dependency.span = span;
        unit.dependencies.push_back(std::move(dependency));
    }

    Type base_type(Context& ctx, const Syntax& at, std::string text, bool legacy) {
        if (auto canonical = canonical_base(text); builtin(canonical)) {
            if (canonical == "bool") require_system_header("stdbool.h", at.span);
            else if (canonical == "size_t" || canonical == "ptrdiff_t")
                require_system_header("stddef.h", at.span);
            else if (canonical.ends_with("_t")) require_system_header("stdint.h", at.span);
            return ir::named(std::move(canonical), at.span);
        }
        if (const auto* symbol = lookup(ctx.module, at, text)) {
            const auto kind = work[symbol->index].decl.kind;
            if (symbol->enumerator || !type_decl(kind))
                fail(at, "'" + text + "' is not a type");
            // The emitter provides record forwards; aliases/enums are not forwardable in C.
            if (kind != DeclKind::record) work[ctx.owner].dependencies.insert(symbol->index);
            return ir::named(symbol->emitted, at.span);
        }
        if (legacy && (text.starts_with("struct ") || text.starts_with("enum "))) {
            if (const auto* symbol = lookup(ctx.module, at, text.substr(text.find(' ') + 1))) {
                const auto expected = text.starts_with("struct ") ? DeclKind::record : DeclKind::enum_;
                if (!symbol->enumerator && work[symbol->index].decl.kind == expected) {
                    if (expected == DeclKind::enum_) work[ctx.owner].dependencies.insert(symbol->index);
                    return ir::named(symbol->emitted, at.span);
                }
            }
        }
        const auto m = origin(ctx.module, at);
        const auto dot = text.find('.');
        const auto key = dot == std::string::npos ? text : text.substr(dot + 1);
        bool cpp_only = dot == std::string::npos && cpp_types[m].contains(text);
        for (const auto& import : program.modules[m].imports)
            if (import.alias == (dot == std::string::npos ? "" : text.substr(0, dot)) &&
                visible(import.target, key) && cpp_types[import.target].contains(key))
                cpp_only = true;
        if (cpp_only)
            fail(at, "C++-only type '" + text + "' cannot appear in the C API");
        if (!legacy) fail(at, "unknown type '" + text + "'");
        auto type = ir::named(std::move(text), at.span);
        type.kind = TypeKind::opaque;
        return type;
    }

    Type legacy_type(Context& ctx, const Syntax& node) {
        const auto text = quoted_text(node);
        std::vector<std::string> words;
        for (std::size_t i = 0; i < text.size();) {
            const auto c = text[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
            if (c == '*') { words.emplace_back("*"); ++i; continue; }
            if (!letter(c)) fail(node, "compound quoted type requires a structured declarator");
            const auto start = i++;
            while (i < text.size() && (letter(text[i]) || digit(text[i]))) ++i;
            words.emplace_back(text.substr(start, i - start));
        }
        std::size_t p = 0;
        std::string base;
        bool is_const = false, is_volatile = false;
        while (p < words.size() && words[p] != "*") {
            if (words[p] == "const") is_const = true;
            else if (words[p] == "volatile") is_volatile = true;
            else {
                if (!base.empty()) base += ' ';
                base += words[p];
            }
            ++p;
        }
        if (base.empty()) fail(node, "quoted type is missing a base type");
        if (!builtin(canonical_base(base)) && !is_identifier(base) &&
            !((base.starts_with("struct ") || base.starts_with("enum ") ||
               base.starts_with("union ")) && is_identifier(std::string_view(base).substr(base.find(' ') + 1))))
            fail(node, "invalid quoted C type '" + base + "'");
        auto type = base_type(ctx, node, std::move(base), true);
        type.is_const = is_const;
        type.is_volatile = is_volatile;
        while (p < words.size()) {
            if (words[p++] != "*") fail(node, "invalid quoted pointer type");
            type = ir::wrapped(TypeKind::pointer, std::move(type), node.span);
            while (p < words.size() && words[p] != "*") {
                if (words[p] == "const") type.is_const = true;
                else if (words[p] == "volatile") type.is_volatile = true;
                else fail(node, "invalid quoted pointer qualifier '" + words[p] + "'");
                ++p;
            }
        }
        return type;
    }

    Type type(Context& ctx, const Syntax& node) {
        if (node.is_atom) {
            if (node.quoted) return legacy_type(ctx, node);
            return base_type(ctx, node, reference(node), false);
        }
        const auto op = head(node);
        if (op == "ptr" || op == "const" || op == "volatile") {
            arity(node, 1, 1);
            auto inner = type(ctx, node.items[1]);
            if (op == "ptr") return ir::wrapped(TypeKind::pointer, std::move(inner), node.span);
            if (op == "const") inner.is_const = true;
            else inner.is_volatile = true;
            return inner;
        }
        if (op == "array") {
            arity(node, 2, 2);
            const auto& size = node.items[2];
            const auto text = atom(size);
            auto result = ir::wrapped(TypeKind::array, type(ctx, node.items[1]), node.span);
            auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), result.extent);
            if (size.quoted || ec != std::errc{} || end != text.data() + text.size() || !result.extent)
                fail(size, "array extent must be a positive integer");
            return result;
        }
        if (op == "function") {
            arity(node, 2, 2);
            auto result = ir::wrapped(TypeKind::function, type(ctx, node.items[1]), node.span);
            const auto& params = node.items[2];
            if (params.is_atom) fail(params, "expected a list of parameter types");
            for (const auto& p : params.items) result.children.push_back(type(ctx, p));
            return result;
        }
        fail(node, "unknown type constructor '" + std::string(op) + "'");
    }

    Type canonical(const Type& input, const Syntax& at) {
        Depth limit(canonical_depth, at);
        if (++canonical_steps > 262144) fail(at, "semantic type expansion limit exceeded");
        if (input.kind == TypeKind::named) {
            if (const auto it = emitted.find(input.name); it != emitted.end() &&
                work[it->second.index].decl.kind == DeclKind::alias) {
                signature(it->second.index);
                auto result = canonical(work[it->second.index].decl.type, at);
                result.is_const |= input.is_const;
                result.is_volatile |= input.is_volatile;
                return result;
            }
        }
        Type result{input.kind, input.name, input.is_const, input.is_volatile, input.extent, {}, input.span};
        for (const auto& child : input.children) result.children.push_back(canonical(child, at));
        return result;
    }

    const Symbol* emitted_type(const Type& t, DeclKind kind) const {
        if (t.kind == TypeKind::named) {
            if (const auto it = emitted.find(t.name); it != emitted.end() &&
                !it->second.enumerator && work[it->second.index].decl.kind == kind)
                return &it->second;
        }
        return nullptr;
    }

    static bool is_void(const Type& t) { return t.kind == TypeKind::named && t.name == "void"; }
    bool integer(const Type& t) const {
        return t.kind == TypeKind::named &&
               (integers.contains(t.name) || emitted_type(t, DeclKind::enum_));
    }
    bool arithmetic(const Type& t) const {
        return integer(t) || (t.kind == TypeKind::named &&
               (t.name == "float" || t.name == "double" || t.name == "long double"));
    }
    bool scalar(const Type& t) const {
        return arithmetic(t) || t.kind == TypeKind::pointer || t.kind == TypeKind::opaque;
    }

    void complete(const Type& type, std::size_t owner, const Syntax& at) {
        if (const auto* rec = emitted_type(type, DeclKind::record)) {
            if (rec->index == owner && work[owner].decl.kind == DeclKind::record)
                fail(at, "record contains itself by value");
            work[owner].dependencies.insert(rec->index);
        } else if (type.kind == TypeKind::array) complete(type.children.front(), owner, at);
    }

    void check_type(const Type& t, std::size_t owner, const Syntax& at) {
        if (t.kind == TypeKind::array) {
            const auto& element = t.children.front();
            if (is_void(element) || element.kind == TypeKind::function)
                fail(at, "array element must have an object type");
            complete(element, owner, at);
        } else if (t.kind == TypeKind::function) {
            if (t.is_const || t.is_volatile) fail(at, "function type cannot be qualified");
            const auto kind = t.children.front().kind;
            if (kind == TypeKind::array || kind == TypeKind::function)
                fail(at, "function cannot return an array or function");
            for (std::size_t i = 1; i < t.children.size(); ++i) {
                if (is_void(t.children[i])) fail(at, "parameter cannot have void type; use ()");
                if (t.children[i].kind == TypeKind::array || t.children[i].kind == TypeKind::function)
                    fail(at, "parameter must use a pointer instead of an array or function type");
            }
        }
        for (const auto& child : t.children) check_type(child, owner, at);
    }

    void object_type(const Type& t, std::size_t owner, const Syntax& at) {
        if (is_void(t) || t.kind == TypeKind::function) fail(at, "expected an object type");
        complete(t, owner, at);
    }

    void signature(std::size_t i) {
        auto& w = work[i];
        if (w.signature == State::done) return;
        if (w.signature == State::busy) fail(*w.node, "cyclic type dependency involving '" + w.decl.name + "'");
        Depth limit(signature_depth, *w.node);
        w.signature = State::busy;
        auto& d = w.decl;
        const auto& a = w.node->items;
        Context ctx{w.module, i, nullptr, {}, {}};
        if (d.kind == DeclKind::alias || d.kind == DeclKind::variable ||
            d.kind == DeclKind::function || d.kind == DeclKind::external) {
            const auto& where = a[d.kind == DeclKind::alias ? 2 : 1];
            d.type = type(ctx, where);
            const auto c = canonical(d.type, where);
            check_type(c, i, where);
            if (d.kind == DeclKind::variable) object_type(c, i, where);
            else if (d.kind != DeclKind::alias) {
                if (c.kind == TypeKind::array || c.kind == TypeKind::function)
                    fail(where, "function cannot return an array or function");
                if (c.is_const || c.is_volatile) fail(where, "function result cannot have top-level qualifiers");
                complete(c, i, where);
            }
        }
        if (d.kind == DeclKind::record || d.kind == DeclKind::function || d.kind == DeclKind::external) {
            const bool record = d.kind == DeclKind::record;
            const auto& params = a[record ? 2 : 3];
            if (params.is_atom || (record && params.items.empty()))
                fail(params, record ? "expected a nonempty field list" : "expected a parameter list");
            std::set<std::string> names_seen;
            for (const auto& p : params.items) {
                if (p.is_atom || p.items.size() != 2)
                    fail(p, record ? "expected (TYPE NAME) field" : "expected (TYPE NAME) parameter");
                auto name = record ? c_identifier(p.items[1]) : local_name(p.items[1]);
                if (!names_seen.insert(name).second) fail(p.items[1], "duplicate name '" + name + "'");
                if (record && emitted.contains(name) &&
                    work[emitted.at(name).index].decl.kind == DeclKind::constant)
                    fail(p.items[1], "field name conflicts with generated macro '" + name + "'");
                auto ty = type(ctx, p.items[0]);
                const auto c = canonical(ty, p.items[0]);
                check_type(c, i, p.items[0]);
                object_type(c, i, p.items[0]);
                if (!record && c.kind == TypeKind::array)
                    fail(p.items[0], "parameter must use a pointer instead of an array type");
                if (!record && emitted.contains(name)) {
                    const auto& other = emitted.at(name);
                    if (work[other.index].decl.kind == DeclKind::constant ||
                        type_decl(work[other.index].decl.kind)) {
                        if (d.kind == DeclKind::function && a[4].is_atom && a[4].quoted)
                            fail(p.items[1], "opaque body parameter shadows a generated macro or type '" + name + "'");
                        name = "am8_local_" + std::to_string(local_number_++);
                    }
                }
                d.parameters.push_back({std::move(ty), name, p.span});
            }
        }
        w.signature_dependencies = w.dependencies;
        w.signature = State::done;
    }

    Type decay(Type t) const {
        if (t.kind == TypeKind::array) {
            const auto span = t.span;
            auto child = std::move(t.children.front());
            child.is_const |= t.is_const;
            child.is_volatile |= t.is_volatile;
            return ir::wrapped(TypeKind::pointer, std::move(child), span);
        }
        if (t.kind == TypeKind::function) {
            const auto span = t.span;
            return ir::wrapped(TypeKind::pointer, std::move(t), span);
        }
        return t;
    }

    static Type unqualified(Type t) {
        t.is_const = t.is_volatile = false;
        return t;
    }

    bool compatible(const Type& destination, const Value& value) const {
        const auto from = decay(value.type);
        if (is_void(destination) || is_void(from) || destination.kind == TypeKind::array) return false;
        if (destination.kind == TypeKind::opaque || from.kind == TypeKind::opaque) return true;
        if (ir::same_type(unqualified(destination), unqualified(from))) return true;
        if (arithmetic(destination) && arithmetic(from) && !emitted_type(destination, DeclKind::enum_))
            return true;
        if (destination.kind == TypeKind::pointer) {
            if (value.null) return true;
            if (from.kind != TypeKind::pointer) return false;
            const auto& a = destination.children.front();
            const auto& b = from.children.front();
            return (!b.is_const || a.is_const) && (!b.is_volatile || a.is_volatile) &&
                   (ir::same_type(unqualified(a), unqualified(b)) ||
                    (is_void(a) && b.kind != TypeKind::function));
        }
        return false;
    }

    bool initializer_compatible(const Type& destination, const Value& value) const {
        if (destination.kind == TypeKind::array && value.expr.kind == ExprKind::string) {
            const auto& element = destination.children.front();
            return element.kind == TypeKind::named && element.name == "char" &&
                   destination.extent > value.expr.text.size();
        }
        return compatible(destination, value) ||
               (destination.kind == TypeKind::array && value.expr.kind == ExprKind::opaque);
    }

    bool modifiable(const Type& ty, const Syntax& at) {
        Depth limit(member_depth, at);
        if (ty.is_const || ty.kind == TypeKind::array) return false;
        if (const auto* rec = emitted_type(ty, DeclKind::record)) {
            signature(rec->index);
            return std::all_of(work[rec->index].decl.parameters.begin(),
                work[rec->index].decl.parameters.end(), [&](const Parameter& field) {
                    auto type = canonical(field.type, at);
                    while (type.kind == TypeKind::array && !type.is_const) {
                        const auto element = type.children.front();
                        type = element;
                    }
                    return modifiable(type, at);
                });
        }
        return true;
    }

    Type callable(Work& w, const Syntax& at) {
        auto result = ir::wrapped(TypeKind::function, canonical(w.decl.type, at), at.span);
        for (const auto& p : w.decl.parameters)
            result.children.push_back(unqualified(canonical(p.type, at)));
        return result;
    }

    const Local* local(const Context& ctx, const std::pair<std::size_t, std::string>& key) const {
        for (auto it = ctx.scopes.rbegin(); it != ctx.scopes.rend(); ++it)
            if (auto found = it->bindings.find(key); found != it->bindings.end()) return &found->second;
        return nullptr;
    }

    Value name_value(Context& ctx, const Syntax& node) {
        const auto name = reference(node);
        Value value{{ExprKind::name, name, {}, node.span}, {}};
        if (const auto* l = local(ctx, {origin(ctx.module, node), name})) {
            value.type = l->type;
            value.expr.text = l->emitted;
            value.lvalue = true;
            return value;
        }
        const auto* symbol = lookup(ctx.module, node, name);
        if (!symbol) fail(node, "unknown name '" + name + "'");
        auto& w = work[symbol->index];
        value.expr.text = symbol->emitted;
        switch (w.decl.kind) {
        case DeclKind::function:
        case DeclKind::external:
            signature(symbol->index);
            work[ctx.owner].prototypes.insert(symbol->index);
            value.type = callable(w, node);
            value.constant = value.static_address = true;
            break;
        case DeclKind::variable:
            signature(symbol->index);
            if (ctx.owner == symbol->index) fail(node, "variable cannot refer to itself in its initializer");
            work[ctx.owner].dependencies.insert(symbol->index);
            value.type = canonical(w.decl.type, node);
            value.lvalue = value.static_address = true;
            value.constant = value.type.kind == TypeKind::array;
            break;
        case DeclKind::constant: {
            initialize(symbol->index);
            const auto& stored = *w.value;
            work[ctx.owner].dependencies.insert(symbol->index);
            value.type = stored.type;
            value.constant = stored.constant;
            value.null = stored.null;
            value.static_address = stored.static_address;
            break;
        }
        case DeclKind::enum_:
            if (!symbol->enumerator) fail(node, "'" + name + "' is not a value");
            if (ctx.owner == symbol->index) {
                if (!ctx.enumerator || *symbol->enumerator >= *ctx.enumerator)
                    fail(node, "enumerator '" + name + "' is not yet defined");
            } else work[ctx.owner].dependencies.insert(symbol->index);
            value.type = ctx.enumerator ? ir::named("int", node.span) : ir::named(w.decl.name, node.span);
            value.constant = true;
            break;
        default:
            fail(node, "'" + name + "' is not a value");
        }
        return value;
    }

    Value opaque_expr(const Syntax& at, std::string text) {
        auto type = ir::named({}, at.span);
        type.kind = TypeKind::opaque;
        return {{ExprKind::opaque, std::move(text), {}, at.span}, std::move(type), false, true};
    }

    Value string_expr(const Syntax& at, std::string text) {
        auto chars = ir::named("char", at.span);
        chars.is_const = true;
        return {{ExprKind::string, std::move(text), {}, at.span},
                ir::wrapped(TypeKind::pointer, std::move(chars), at.span), false, true};
    }

    void needs_condition(const Value& value, const Syntax& at) {
        if (!scalar(decay(value.type))) fail(at, "condition must have scalar type");
    }

    void pointer_arithmetic(const Type& pointer, Context& ctx, const Syntax& at) {
        const auto& pointee = pointer.children.front();
        if (is_void(pointee) || pointee.kind == TypeKind::function)
            fail(at, "pointer arithmetic requires an object pointer");
        complete(pointee, ctx.owner, at);
    }

    Value expression(Context& ctx, const Syntax& node) {
        if (node.is_atom) {
            if (node.quoted) return string_expr(node, node.text);
            if (const auto number = numeric_type(node.text)) {
                auto digits = std::string_view(node.text);
                if (digits.starts_with("0x") || digits.starts_with("0X")) digits.remove_prefix(2);
                const auto suffix = digits.find_first_of("uUlL");
                const auto zero = integers.contains(*number) &&
                                  digits.substr(0, suffix).find_first_not_of('0') == std::string_view::npos;
                return {{ExprKind::number, node.text, {}, node.span}, ir::named(*number, node.span),
                        false, true, zero};
            }
            if (node.text == "true" || node.text == "false")
                return {{ExprKind::number, node.text == "true" ? "1" : "0", {}, node.span},
                        ir::named("bool", node.span), false, true};
            return name_value(ctx, node);
        }
        const auto op = head(node);
        const auto& a = node.items;
        if (op == "raw") { arity(node, 1, 1); return opaque_expr(node, quoted_text(a[1])); }
        if (op == "string") { arity(node, 1, 1); return string_expr(node, quoted_text(a[1])); }
        if (op == "call") {
            arity(node, 1, a.size());
            auto fn = name_value(ctx, a[1]);
            auto ty = decay(fn.type);
            if (ty.kind != TypeKind::pointer || ty.children.front().kind != TypeKind::function)
                fail(a[1], "'" + a[1].text + "' is not callable");
            const auto& signature = ty.children.front().children;
            if (a.size() - 2 != signature.size() - 1)
                fail(node, "call to '" + a[1].text + "' expects " +
                           std::to_string(signature.size() - 1) + " argument(s)");
            Expr e{ExprKind::call, fn.expr.text, {}, node.span};
            for (std::size_t i = 2; i < a.size(); ++i) {
                auto arg = expression(ctx, a[i]);
                if (!compatible(signature[i - 1], arg))
                    fail(a[i], "argument type mismatch in call to '" + a[1].text + "'");
                e.children.push_back(std::move(arg.expr));
            }
            return {std::move(e), unqualified(signature.front())};
        }
        if (op == "field" || op == "ptr-field") {
            arity(node, 2, 2);
            auto object = expression(ctx, a[1]);
            auto ty = object.type;
            if (op == "ptr-field") {
                ty = decay(std::move(ty));
                if (ty.kind != TypeKind::pointer) fail(a[1], "ptr-field requires a record pointer");
                const auto pointee = ty.children.front();
                ty = pointee;
            }
            const auto* record = emitted_type(ty, DeclKind::record);
            if (!record) fail(a[1], "field access requires a known record type");
            signature(record->index);
            complete(ty, ctx.owner, a[1]);
            const auto field = c_identifier(a[2]);
            for (const auto& p : work[record->index].decl.parameters) {
                if (p.name != field) continue;
                auto field_type = canonical(p.type, a[2]);
                field_type.is_const |= ty.is_const;
                field_type.is_volatile |= ty.is_volatile;
                return {{op == "field" ? ExprKind::field : ExprKind::ptr_field, field,
                         {std::move(object.expr)}, node.span}, std::move(field_type),
                        op == "ptr-field" || object.lvalue, false, false,
                        op == "field" && object.static_address};
            }
            fail(a[2], "unknown field '" + field + "'");
        }
        if (op == "index") {
            arity(node, 2, 2);
            auto object = expression(ctx, a[1]);
            auto index = expression(ctx, a[2]);
            auto ty = decay(object.type);
            if (ty.kind != TypeKind::pointer || (!integer(index.type) && index.type.kind != TypeKind::opaque))
                fail(node, "index requires an object pointer or array and an integer index");
            pointer_arithmetic(ty, ctx, a[1]);
            return {{ExprKind::index, {}, {std::move(object.expr), std::move(index.expr)}, node.span},
                    ty.children.front(), true, false,
                    false, object.type.kind == TypeKind::array && object.static_address && index.constant};
        }
        static const std::set<std::string_view> unary{"+", "-", "!", "~", "&", "*"};
        static const std::set<std::string_view> binary{
            "+", "-", "*", "/", "%", "<<", ">>", "<", "<=", ">", ">=", "==", "!=",
            "&", "|", "^", "&&", "||"
        };
        if (unary.contains(op) && a.size() == 2) {
            auto operand = expression(ctx, a[1]);
            auto ty = operand.type;
            bool lvalue = false;
            bool address = false;
            bool constant = operand.constant;
            if (op == "&") {
                if (!operand.lvalue && ty.kind != TypeKind::function)
                    fail(a[1], "address-of requires an lvalue or function");
                ty = ir::wrapped(TypeKind::pointer, std::move(ty), node.span);
                constant = operand.static_address;
            } else if (op == "*") {
                ty = decay(std::move(ty));
                if (ty.kind != TypeKind::pointer || is_void(ty.children.front()))
                    fail(a[1], "dereference requires a non-void pointer");
                const auto pointee = ty.children.front();
                ty = pointee;
                lvalue = ty.kind != TypeKind::function;
                address = operand.constant;
                constant = ty.kind == TypeKind::function && operand.constant;
            } else if (op == "!") {
                needs_condition(operand, a[1]);
                ty = ir::named("int", node.span);
            } else if (ty.kind != TypeKind::opaque && !(op == "~" ? integer(ty) : arithmetic(ty))) {
                fail(a[1], "invalid operand type for unary '" + std::string(op) + "'");
            } else if (integer(ty) && (ty.name == "bool" || ty.name == "char" ||
                       ty.name == "signed char" || ty.name == "unsigned char" ||
                       ty.name == "short" || ty.name == "unsigned short" ||
                       emitted_type(ty, DeclKind::enum_))) {
                ty = ir::named("int", node.span);
            }
            return {{ExprKind::unary, std::string(op), {std::move(operand.expr)}, node.span},
                    std::move(ty), lvalue, constant, false, address};
        }
        if (binary.contains(op)) {
            arity(node, 2, 2);
            return binary_expression(ctx, node, op);
        }
        if (unary.contains(op)) arity(node, 1, 1);
        fail(node, "unknown expression '" + std::string(op) + "'");
    }

    bool pointer_compare(const Type& a, const Type& b, bool equality) const {
        if (a.kind != TypeKind::pointer || b.kind != TypeKind::pointer) return false;
        const auto& x = a.children.front();
        const auto& y = b.children.front();
        if (!equality && (is_void(x) || is_void(y) || x.kind == TypeKind::function ||
                         y.kind == TypeKind::function)) return false;
        return ir::same_type(unqualified(x), unqualified(y)) ||
               (equality && ((is_void(x) && y.kind != TypeKind::function) ||
                             (is_void(y) && x.kind != TypeKind::function)));
    }

    Value binary_expression(Context& ctx, const Syntax& node, std::string_view op) {
        const auto& a = node.items;
        auto lhs = expression(ctx, a[1]);
        auto rhs = expression(ctx, a[2]);
        const auto l = decay(lhs.type), r = decay(rhs.type);
        const bool opaque = l.kind == TypeKind::opaque || r.kind == TypeKind::opaque;
        auto result = ir::named("int", node.span);
        bool constant = lhs.constant && rhs.constant;
        if (op == "&&" || op == "||") {
            needs_condition(lhs, a[1]);
            needs_condition(rhs, a[2]);
            if (l.kind == TypeKind::pointer || r.kind == TypeKind::pointer) constant = false;
        } else if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
            const bool eq = op == "==" || op == "!=";
            if (!opaque && !(arithmetic(l) && arithmetic(r)) && !pointer_compare(l, r, eq) &&
                !(eq && ((l.kind == TypeKind::pointer && rhs.null) ||
                          (r.kind == TypeKind::pointer && lhs.null))))
                fail(node, "incompatible comparison operands");
            if (l.kind == TypeKind::pointer || r.kind == TypeKind::pointer) constant = false;
        } else if ((op == "+" || op == "-") && l.kind == TypeKind::pointer && integer(r)) {
            pointer_arithmetic(l, ctx, a[1]);
            result = l;
        } else if (op == "+" && integer(l) && r.kind == TypeKind::pointer) {
            pointer_arithmetic(r, ctx, a[2]);
            result = r;
        } else if (op == "-" && pointer_compare(l, r, false)) {
            pointer_arithmetic(l, ctx, a[1]);
            result = ir::named("long", node.span);
            constant = false;
        } else {
            const bool int_only = op == "%" || op == "<<" || op == ">>" ||
                                  op == "&" || op == "|" || op == "^";
            if (opaque) result.kind = TypeKind::opaque;
            else if (!(int_only ? integer(l) && integer(r) : arithmetic(l) && arithmetic(r)))
                fail(node, "invalid operand types for '" + std::string(op) + "'");
            else {
                // Usual arithmetic conversions are performed by C/C++. Track the
                // expression category and floating rank needed for subsequent checks.
                static const std::vector<std::string_view> ranks{"long double", "double", "float"};
                for (auto rank : ranks)
                    if (l.name == rank || r.name == rank) { result.name = rank; break; }
            }
        }
        return {{ExprKind::binary, std::string(op), {std::move(lhs.expr), std::move(rhs.expr)}, node.span},
                std::move(result), false, constant};
    }

    std::pair<Stmt, bool> statement(Context& ctx, const Syntax& node, bool block_scope = true) {
        const auto op = head(node);
        const auto& a = node.items;
        Stmt s;
        s.span = node.span;
        bool returns = false;
        if (node.is_atom && node.quoted) {
            s.kind = StmtKind::opaque;
            s.text = node.text;
        } else if (op == "block") {
            s.kind = StmtKind::block;
            if (block_scope) ctx.scopes.emplace_back();
            for (std::size_t i = 1; i < a.size(); ++i) {
                auto [child, terminates] = statement(ctx, a[i]);
                returns |= terminates;
                s.children.push_back(std::move(child));
            }
            if (block_scope) ctx.scopes.pop_back();
        } else if (op == "let") {
            arity(node, 3, 3);
            s.kind = StmtKind::local;
            s.type = type(ctx, a[1]);
            auto ty = canonical(s.type, a[1]);
            check_type(ty, ctx.owner, a[1]);
            object_type(ty, ctx.owner, a[1]);
            const auto name = local_name(a[2]);
            const auto key = std::pair{origin(ctx.module, a[2]), name};
            s.text = name;
            if (ctx.scopes.back().bindings.contains(key)) fail(a[2], "duplicate local '" + name + "'");
            auto value = expression(ctx, a[3]);
            if (!initializer_compatible(ty, value)) fail(a[3], "initializer type mismatch for local '" + s.text + "'");
            bool collides = emitted.contains(name);
            for (const auto& scope : ctx.scopes)
                collides |= scope.emitted_names.contains(name);
            if (collides) s.text = "am8_local_" + std::to_string(local_number_++);
            s.expressions.push_back(std::move(value.expr));
            ctx.scopes.back().bindings.emplace(key, Local{std::move(ty), s.text});
            ctx.scopes.back().emitted_names.insert(s.text);
        } else if (op == "set") {
            arity(node, 2, 2);
            s.kind = StmtKind::assign;
            auto target = expression(ctx, a[1]);
            if (!target.lvalue || !modifiable(target.type, a[1]))
                fail(a[1], "assignment target is not a writable lvalue");
            auto value = expression(ctx, a[2]);
            if (!compatible(target.type, value)) fail(a[2], "assignment type mismatch");
            s.expressions = {std::move(target.expr), std::move(value.expr)};
        } else if (op == "if" || op == "while") {
            arity(node, 2, op == "if" ? 3 : 2);
            s.kind = op == "if" ? StmtKind::branch : StmtKind::loop;
            auto condition = expression(ctx, a[1]);
            needs_condition(condition, a[1]);
            s.expressions.push_back(std::move(condition.expr));
            returns = op == "if" && a.size() == 4;
            for (std::size_t i = 2; i < a.size(); ++i) {
                ctx.scopes.emplace_back();
                auto [child, terminates] = statement(ctx, a[i], false);
                ctx.scopes.pop_back();
                returns &= terminates;
                s.children.push_back(std::move(child));
            }
        } else if (op == "return") {
            arity(node, 0, 1);
            s.kind = StmtKind::return_;
            returns = true;
            if (a.size() == 1 && !is_void(*ctx.result))
                fail(node, "non-void function must return a value");
            if (a.size() == 2) {
                if (is_void(*ctx.result)) fail(node, "void function cannot return a value");
                auto value = expression(ctx, a[1]);
                if (!compatible(*ctx.result, value)) fail(a[1], "return type mismatch");
                s.expressions.push_back(std::move(value.expr));
            }
        } else if (op == "expr") {
            arity(node, 1, 1);
            s.kind = StmtKind::expression;
            s.expressions.push_back(expression(ctx, a[1]).expr);
        } else if (op == "raw") {
            arity(node, 1, 1);
            s.kind = StmtKind::opaque;
            s.text = quoted_text(a[1]);
        } else fail(node, "unknown statement '" + std::string(op) + "'");
        return {std::move(s), returns};
    }

    static bool has_opaque(const Stmt& stmt) {
        return stmt.kind == StmtKind::opaque ||
               std::any_of(stmt.children.begin(), stmt.children.end(), has_opaque);
    }

    void initialize(std::size_t i) {
        auto& w = work[i];
        if (w.initializer == State::done) return;
        if (w.initializer == State::busy)
            fail(*w.node, "cyclic definition of '" + w.decl.name + "'");
        Depth limit(initializer_depth, *w.node);
        w.initializer = State::busy;
        auto& d = w.decl;
        const auto& a = w.node->items;
        Context ctx{w.module, i, nullptr, {{}}, {}};
        if (d.kind == DeclKind::constant || d.kind == DeclKind::variable) {
            const auto& init = a[d.kind == DeclKind::constant ? 2 : 3];
            const bool legacy = d.kind == DeclKind::constant &&
                (init.quoted || (init.is_atom && is_identifier(init.text) && !reserved.contains(init.text) &&
                                 !lookup(ctx.module, init, init.text)));
            auto value = legacy
                       ? opaque_expr(init, init.text) : expression(ctx, init);
            if (d.kind == DeclKind::variable) {
                if (!initializer_compatible(canonical(d.type, a[1]), value))
                    fail(init, "initializer type mismatch for variable '" + d.name + "'");
                if (!value.constant) fail(init, "global variable initializer must be a C constant expression");
            }
            d.initializer = value.expr;
            w.value = std::move(value);
        } else if (d.kind == DeclKind::enum_) {
            for (std::size_t j = 0; j < w.enum_values.size(); ++j) {
                if (const auto* init = w.enum_values[j]) {
                    ctx.enumerator = j;
                    auto value = expression(ctx, *init);
                    if (!value.constant || (!integer(value.type) && value.type.kind != TypeKind::opaque))
                        fail(*init, "enumerator initializer must be an integer constant expression");
                    d.enumerators[j].second = std::move(value.expr);
                }
            }
        } else if (d.kind == DeclKind::function) {
            const auto& body = a[4];
            auto result = canonical(d.type, a[1]);
            ctx.result = &result;
            for (std::size_t j = 0; j < d.parameters.size(); ++j) {
                const auto& p = d.parameters[j];
                const auto& param = a[3].items[j].items[1];
                const auto key = std::pair{origin(ctx.module, param), param.text};
                ctx.scopes.front().bindings.emplace(key, Local{canonical(p.type, param), p.name});
                ctx.scopes.front().emitted_names.insert(p.name);
            }
            auto [stmt, returns] = statement(ctx, body, false);
            if (!is_void(result) && !returns && !has_opaque(stmt))
                fail(body, "non-void function may reach the end without returning a value");
            d.body = std::move(stmt);
        }
        w.initializer = State::done;
    }

    void check_entry() {
        if (!entry) return;
        if (entry->is_atom && entry->quoted) {
            unit.entry = Entry{entry->text, true, entry->span};
            return;
        }
        const auto spelling = reference(*entry);
        const auto* sym = lookup(program.root, *entry, spelling);
        if (!sym || sym->enumerator || work[sym->index].decl.kind != DeclKind::function)
            fail(*entry, "CLI entry must name a defined function");
        auto& d = work[sym->index].decl;
        auto result = ir::named("int");
        auto argv = ir::wrapped(TypeKind::pointer, ir::wrapped(TypeKind::pointer, ir::named("char")));
        if (d.parameters.size() != 2 ||
            !ir::same_type(unqualified(canonical(d.type, *entry)), result) ||
            !ir::same_type(unqualified(canonical(d.parameters[0].type, *entry)), result) ||
            !ir::same_type(unqualified(canonical(d.parameters[1].type, *entry)), argv))
            fail(*entry, "CLI entry must have signature int (int, char **)");
        unit.entry = Entry{d.name, false, entry->span};
    }

    void order() {
        const auto n = work.size();
        // Node 2*i precedes body 2*i+1 in the ready set. Dependencies on
        // callable names target a prototype, never the function definition.
        std::vector<std::vector<std::size_t>> dependencies(2 * n);
        std::vector<bool> present(2 * n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& w = work[i];
            present[2 * i + 1] = true;
            for (auto d : w.dependencies) dependencies[2 * i + 1].push_back(2 * d + 1);
            for (auto d : w.prototypes)
                dependencies[2 * i + 1].push_back(2 * d + (work[d].decl.kind == DeclKind::external));
            if (w.decl.kind == DeclKind::function) {
                present[2 * i] = true;
                for (auto d : w.signature_dependencies) dependencies[2 * i].push_back(2 * d + 1);
                dependencies[2 * i + 1].push_back(2 * i);
            }
        }
        std::vector<std::vector<std::size_t>> users(2 * n);
        std::vector<std::size_t> missing(2 * n);
        std::set<std::size_t> ready;
        for (std::size_t i = 0; i < 2 * n; ++i) {
            if (!present[i]) continue;
            missing[i] = dependencies[i].size();
            if (missing[i] == 0) ready.insert(i);
            for (auto d : dependencies[i]) users[d].push_back(i);
        }
        while (!ready.empty()) {
            const auto i = *ready.begin();
            ready.erase(ready.begin());
            auto& w = work[i / 2];
            if (i % 2 == 0) {
                Decl proto;
                proto.kind = DeclKind::function;
                proto.name = w.decl.name;
                proto.type = w.decl.type;
                proto.parameters = w.decl.parameters;
                proto.span = w.decl.span;
                proto.exported = w.decl.exported;
                unit.declarations.push_back(std::move(proto));
            } else {
                if (w.decl.kind == DeclKind::external) {
                    auto function = ir::wrapped(TypeKind::function, std::move(w.decl.type), w.decl.span);
                    for (const auto& p : w.decl.parameters) function.children.push_back(p.type);
                    w.decl.type = std::move(function);
                }
                unit.declarations.push_back(std::move(w.decl));
            }
            for (auto user : users[i])
                if (--missing[user] == 0) ready.insert(user);
        }
        if (std::any_of(missing.begin(), missing.end(), [](auto n) { return n != 0; })) {
            auto i = std::size_t(std::find_if(missing.begin(), missing.end(), [](auto n) { return n != 0; }) -
                                 missing.begin());
            std::set<std::size_t> visited;
            while (visited.insert(i).second) {
                const auto it = std::find_if(dependencies[i].begin(), dependencies[i].end(),
                    [&](auto dependency) { return missing[dependency] != 0; });
                i = *it;
            }
            const auto& name = work[i / 2].decl.name;
            fail(*work[i / 2].node, "cyclic declaration dependency involving '" +
                                (c_names.contains(name) ? c_names.at(name) : name) + "'");
        }
    }

public:
    Lowerer(const Program& p, std::vector<Dependency> dependencies)
        : program(p), names(p.modules.size()), cpp_types(p.modules.size()) {
        if (p.root >= p.modules.size()) fail(Span{}, "invalid root module");
        unit.name = p.modules[p.root].name;
        unit.span = p.modules[p.root].span;
        unit.dependencies = std::move(dependencies);
        for (const auto& module : p.modules)
            for (const auto& import : module.imports)
                if (import.target >= p.modules.size()) fail(import.span, "invalid imported module");
    }

    Unit run() {
        collect();
        for (std::size_t m = 0; m < names.size(); ++m)
            for (const auto& name : program.modules[m].exports)
                if (!names[m].contains(name) && !cpp_types[m].contains(name) &&
                    !program.modules[m].compile_time_exports.contains(name))
                    fail(program.modules[m].span, "unknown exported name '" + name + "'");
        for (std::size_t i = 0; i < work.size(); ++i) signature(i);
        for (std::size_t i = 0; i < work.size(); ++i) initialize(i);
        check_entry();
        order();
        return std::move(unit);
    }
};

} // namespace am8::semantic

namespace am8 {

inline Unit lower(const Program& program, std::vector<Dependency> dependencies = {}) {
    return semantic::Lowerer(program, std::move(dependencies)).run();
}

} // namespace am8
