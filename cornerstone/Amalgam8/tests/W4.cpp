#include "../Emitter.hpp"

#include <filesystem>
#include <fstream>

using namespace am8;

namespace {

Type named(std::string spelling) {
    Type t;
    t.name = std::move(spelling);
    return t;
}

Type compound(TypeKind kind, std::vector<Type> children, std::size_t extent = 0) {
    Type t;
    t.kind = kind;
    t.extent = extent;
    t.children = std::move(children);
    return t;
}

Type ptr(Type pointee) { return compound(TypeKind::pointer, {std::move(pointee)}); }

Decl decl(DeclKind kind, std::string name = {}, Type type = {}) {
    Decl d;
    d.kind = kind;
    d.name = std::move(name);
    d.type = std::move(type);
    return d;
}

Stmt opaque_body(std::string source) {
    Stmt s;
    s.kind = StmtKind::opaque;
    s.text = std::move(source);
    return s;
}

Expr expr(ExprKind kind, std::string text, std::vector<Expr> children = {}) {
    return {kind, std::move(text), std::move(children), {}};
}

Stmt stmt(StmtKind kind, std::vector<Expr> expressions, std::vector<Stmt> children = {}) {
    Stmt result;
    result.kind = kind;
    result.expressions = std::move(expressions);
    result.children = std::move(children);
    return result;
}

void write(const fs::path& path, const std::string& source) {
    std::ofstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.open(path, std::ios::binary);
    out << source;
    out.close();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path dir(argv[1]);
    Unit unit;
    unit.name = "W4Emitter";
    auto& declarations = unit.declarations;
    Dependency dep;
    dep.kind = DependencyKind::system;
    dep.header = "stddef.h";
    unit.dependencies.push_back(dep);
    dep.header = "not-a-real-header.h";
    dep.context = Context::compiler;
    unit.dependencies.push_back(dep);
    dep.kind = DependencyKind::bundled;
    dep.context = Context::cpp;
    dep.bundled_source = "template <class T> struct W4CppOnly {};";
    dep.adapter_source = "typedef W4CppOnly<int> W4Adapter;";
    unit.dependencies.push_back(dep);

    const auto integer = named("int");
    const auto callback_function = compound(TypeKind::function, {integer, integer});
    declarations.push_back(decl(DeclKind::alias, "Callback", ptr(callback_function)));
    declarations.push_back(decl(DeclKind::alias, "NodePtr", ptr(named("Node"))));
    auto node = decl(DeclKind::record, "Node");
    auto row = compound(TypeKind::array, {integer}, 3);
    auto const_row_ptr = ptr(row);
    const_row_ptr.is_const = true;
    node.parameters = {{ptr(named("Node")), "next", {}}, {row, "row", {}},
                       {const_row_ptr, "row_ptr", {}},
                       {compound(TypeKind::array, {ptr(callback_function)}, 2), "callbacks", {}}};
    declarations.push_back(node);
    auto enum_ = decl(DeclKind::enum_, "Status");
    enum_.enumerators = {{"READY", Expr{ExprKind::number, "1", {}, {}}}};
    declarations.push_back(enum_);

    declarations.push_back(decl(DeclKind::external, "external_value", integer));
    declarations.push_back(decl(DeclKind::external, "increment", callback_function));
    auto state = decl(DeclKind::variable, "private_state", integer);
    state.initializer = Expr{ExprKind::number, "40", {}, {}};
    declarations.push_back(state);
    state.name = "unused_state";
    declarations.push_back(state);
    auto unused_function = decl(DeclKind::function, "unused_function", integer);
    unused_function.body = opaque_body("return 0;");
    declarations.push_back(unused_function);

    auto raw = decl(DeclKind::raw_c);
    raw.raw = "typedef int raw_type;";
    declarations.push_back(raw);
    raw.kind = DeclKind::raw_cpp;
    raw.raw = "using raw_cpp_type = W4Adapter;";
    declarations.push_back(raw);

    auto counter = decl(DeclKind::function, "counter", named("raw_type"));
    declarations.push_back(counter); // Prototype before the first reference.

    auto call_counter = decl(DeclKind::function, "call_counter", integer);
    Stmt return_call;
    return_call.kind = StmtKind::return_;
    return_call.expressions.push_back({ExprKind::call, "counter", {}, {}});
    call_counter.body = return_call;
    declarations.push_back(call_counter);

    counter.body = opaque_body("return ++private_state;");
    declarations.push_back(counter);
    auto get_callback = decl(DeclKind::function, "get_callback", ptr(callback_function));
    get_callback.body = opaque_body("return increment;");
    declarations.push_back(get_callback);

    auto chars = named("char");
    chars.is_const = true;
    auto escape = decl(DeclKind::function, "escaped", ptr(chars));
    Stmt ret;
    ret.kind = StmtKind::return_;
    ret.expressions.push_back({ExprKind::string, std::string("??") + "/\"\\\n\1" + "7", {}, {}});
    escape.body = ret;
    declarations.push_back(escape);

    auto evaluate = decl(DeclKind::function, "evaluate", integer);
    evaluate.parameters = {{ptr(named("Node")), "p", {}}};
    const auto value = expr(ExprKind::name, "value");
    const auto one = expr(ExprKind::binary, "-", {
        expr(ExprKind::binary, "*", {
            expr(ExprKind::number, "2"), expr(ExprKind::opaque, "3 - 2")}),
        expr(ExprKind::number, "1")});
    const auto zero = expr(ExprKind::number, "0");
    auto local = stmt(StmtKind::local, {expr(ExprKind::opaque, "{0}")});
    local.type = integer;
    local.text = "value";
    const auto node_ptr = expr(ExprKind::name, "p");
    const auto limit = expr(ExprKind::index, "", {
        expr(ExprKind::field, "row", {expr(ExprKind::unary, "*", {node_ptr})}), zero});
    const auto assign = stmt(StmtKind::assign, {value, expr(ExprKind::binary, "+", {value, one})});
    const auto loop = stmt(StmtKind::loop, {expr(ExprKind::binary, "<", {value, limit})}, {assign});
    const auto branch = stmt(StmtKind::branch, {expr(ExprKind::ptr_field, "next", {node_ptr})},
                             {stmt(StmtKind::return_, {zero}), loop});
    evaluate.body = stmt(StmtKind::block, {}, {local, branch, stmt(StmtKind::return_, {value})});
    declarations.push_back(evaluate);

    const auto api = emit(unit, Mode::api);
    if (api != emit(unit, Mode::api) || api.find("main(") != std::string::npos) return 1;
    write(dir / "api.h", api);
    try {
        (void)emit(unit, Mode::cli);
        return 1;
    } catch (const Error&) {
        // A CLI needs an entry; API emission does not.
    }

    const std::string check = "call_counter() == 41 && get_callback()(0) == 0 && "
                              "escaped()[0] == '?' && evaluate(&n) == 4";
    const std::string node_set = "Node n = {NULL, {4, 0, 0}, NULL, {NULL, NULL}};\n";
    unit.entry = Entry{node_set + "return " + check + " ? 0 : 1;", true, {}};
    write(dir / "cli_raw.c", emit(unit, Mode::cli));
    auto entry = decl(DeclKind::function, "run", integer);
    entry.parameters = {{integer, "argc", {}}, {ptr(ptr(named("char"))), "argv", {}}};
    entry.body = opaque_body(node_set + "return argc > 0 && argv != NULL && " + check + " ? 0 : 1;");
    declarations.push_back(entry);
    unit.entry = Entry{"run", false, {}};
    write(dir / "cli_entry.c", emit(unit, Mode::cli));
    write(dir / "cli_entry.h", emit(unit, Mode::api));
}
