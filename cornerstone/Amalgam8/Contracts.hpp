#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace am8 {

namespace fs = std::filesystem;
using Error = std::runtime_error;

struct Span {
    std::string file;
    std::size_t begin{}, end{}, line{1}, column{1};
};

struct Syntax {
    bool is_atom{};
    std::string text;
    std::vector<Syntax> items;
    bool quoted{};
    Span span;
    std::vector<Span> provenance;
    std::optional<std::size_t> origin_module;
};

inline std::string location(const Span& s) {
    return (s.file.empty() ? "<input>" : s.file) + ":" +
           std::to_string(s.line) + ":" + std::to_string(s.column);
}
[[noreturn]] inline void fail(const Span& where, std::string message) {
    throw Error(location(where) + ": " + message);
}
[[noreturn]] inline void fail(const Syntax& where, std::string message) {
    for (const auto& note : where.provenance)
        message += "\n" + location(note) + ": note: expanded from here";
    fail(where.span, std::move(message));
}
inline std::string_view head(const Syntax& x) {
    if (x.is_atom || x.items.empty() || !x.items.front().is_atom || x.items.front().quoted)
        return {};
    return x.items.front().text;
}
inline std::string atom(const Syntax& x) {
    if (!x.is_atom || x.text.empty()) fail(x, "expected nonempty atom");
    return x.text;
}
inline bool letter(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_identifier(std::string_view text) {
    return !text.empty() && letter(text.front()) &&
           std::all_of(text.begin(), text.end(), [](char c) { return letter(c) || digit(c); });
}
inline std::string identifier(const Syntax& x) {
    const auto name = atom(x);
    if (x.quoted || !is_identifier(name)) fail(x, "invalid identifier '" + name + "'");
    return name;
}
inline std::string quoted_text(const Syntax& x) {
    if (!x.is_atom || !x.quoted) fail(x, "expected quoted text");
    return x.text;
}
inline std::string include_path(const Syntax& x) {
    auto path = atom(x);
    const fs::path parsed(path);
    if (parsed.is_absolute() || parsed.filename().empty() ||
        !std::all_of(path.begin(), path.end(), [](char c) {
            return letter(c) || digit(c) || c == '/' || c == '.' || c == '-';
        }))
        fail(x, "invalid include path '" + path + "'");
    for (const auto& part : parsed)
        if (part == "." || part == "..") fail(x, "invalid include component in '" + path + "'");
    return path;
}

struct Limits {
    std::size_t source_bytes = 256 * 1024;
    std::size_t syntax_nodes = 65536;
    std::size_t syntax_depth = 128;
    std::size_t modules = 128;
    std::size_t expansion_depth = 128;
    std::size_t rewrites = 4096;
    std::size_t generated_nodes = 65536;
    std::size_t stage_steps = 65536;
    std::size_t expansion_work = 4 * 1024 * 1024;
};

struct Import { std::size_t target{}; std::string alias; Span span; };
struct Module {
    std::string name;
    Span span;
    std::vector<Syntax> forms;
    std::vector<Import> imports;
    std::set<std::string> exports;
    std::set<std::string> compile_time_exports;
};
struct Program { std::vector<Module> modules; std::size_t root{}; };

struct NativeForm {
    std::string name;
    std::size_t min_arity{};
    std::optional<std::size_t> max_arity;
    std::function<void(const Syntax&)> validate;
    std::function<std::vector<Syntax>(const Syntax&)> lower;
    std::vector<std::string> capabilities;
};
class NativeRegistry {
public:
    NativeRegistry& add(NativeForm);
    const NativeForm* find(std::string_view) const;
private:
    std::map<std::string, NativeForm, std::less<>> forms_;
};

enum class Mode { api, cli };
enum class Context { c, cpp, compiler };
enum class DependencyKind { system, external, bundled };
struct Dependency {
    std::string name;                 // Catalog ID (family:component), or system header.
    std::string header;               // Public include spelling.
    Context context{Context::c};
    DependencyKind kind{DependencyKind::external};
    Span span;
    std::vector<std::string> requires_names;
    std::vector<std::string> include_roots;
    std::vector<std::string> libraries;
    std::string conditions;
    std::string bundled_source;
    std::string adapter_source;      // Trusted implementation accompanying a native header.
};
struct Options {
    Mode mode{Mode::api};
    Limits limits;
    NativeRegistry native;
    bool bundle{};
    fs::path include_root;
};
struct OpaqueFragment {
    Context context;
    std::string_view role;           // Fixed label, never text from the fragment.
    Span span;
};
struct Result {
    std::string source;
    std::vector<Dependency> dependencies;
    std::vector<OpaqueFragment> opaque_fragments;
};

enum class TypeKind { named, pointer, array, function, opaque };
struct Type {
    TypeKind kind{TypeKind::named};
    std::string name;                 // Base/opaque spelling.
    bool is_const{}, is_volatile{};
    std::size_t extent{};
    // One element for pointer/array; result followed by parameters for function.
    std::vector<Type> children;
    Span span;
};
struct Parameter { Type type; std::string name; Span span; };

enum class ExprKind { name, number, string, call, unary, binary, field, ptr_field, index, opaque };
struct Expr {
    ExprKind kind{ExprKind::name};
    std::string text;                 // Identifier, literal, operator, field, callee or raw text.
    std::vector<Expr> children;
    Span span;
};
enum class StmtKind { block, local, assign, branch, loop, return_, expression, opaque };
struct Stmt {
    StmtKind kind{StmtKind::block};
    std::string text;                 // Local name or opaque C text.
    Type type;                        // Local type.
    std::vector<Expr> expressions;
    std::vector<Stmt> children;
    Span span;
};

enum class DeclKind { constant, alias, record, enum_, function, variable, external, raw_c, raw_cpp };
struct Decl {
    DeclKind kind{DeclKind::constant};
    std::string name;
    Type type;                        // Result, variable, or alias type.
    std::vector<Parameter> parameters; // Function parameters / record fields.
    std::vector<std::pair<std::string, std::optional<Expr>>> enumerators;
    std::optional<Expr> initializer;
    std::optional<Stmt> body;
    std::string raw;
    Span span;
    bool exported{true};
};
struct Entry { std::string text; bool raw{}; Span span; };
struct Unit {
    std::string name;
    Span span;
    std::vector<Dependency> dependencies;
    std::vector<Decl> declarations;
    std::optional<Entry> entry;
};

} // namespace am8
