#include "Internal.hpp"

namespace gateml {
namespace {
using Registers = std::map<std::string, std::uint8_t>;
struct NativeFunction;
using Functions = std::map<std::string, std::shared_ptr<NativeFunction>>;
struct NativeFunction {
    std::string label, name;
    std::vector<std::string> parameters;
    Tree body;
    Registers captured;
    Functions functions;
    bool recursive = false, queued = false, print = false;
};
bool used(const Tree& e, const std::string& name) {
    if (e->kind == Expr::Variable) return e->text == name;
    if (e->kind == Expr::Lambda && e->text == name) return false;
    if (e->kind == Expr::Let && e->text == name)
        return !e->recursive && used(e->children[0], name);
    if (e->kind == Expr::Match) {
        if (used(e->children[0], name)) return true;
        for (std::size_t n = 0; n < e->patterns.size(); ++n) {
            std::function<bool(const Pattern&)> binds = [&](const Pattern& p) {
                if (p.kind == Pattern::Bind && p.text == name) return true;
                for (const auto& c : p.children) if (binds(c)) return true;
                return false;
            };
            if (!binds(e->patterns[n]) && used(e->children[n + 1], name)) return true;
        }
        return false;
    }
    for (const auto& c : e->children) if (used(c, name)) return true;
    return false;
}
class Native {
    metail::Builder builder;
    std::vector<std::shared_ptr<NativeFunction>> pending;
    std::size_t serial = 0;
    const Options& options;
    std::string label() { return "gml_" + std::to_string(serial++); }
    void emit(metail::Op op, std::uint8_t a = 0, std::uint8_t b = 0, std::uint8_t c = 0) {
        builder.emit({op, a, b, c, 0});
    }
    void expression(const Tree& e, Registers env, Functions functions, unsigned next = 2) {
        using O = metail::Op;
        auto child = [&](std::size_t n) { expression(e->children[n], env, functions, next); };
        auto unsupported = [&]() -> void {
            throw Error(e->loc, "native backend requires fully applied, first-order scalar functions and int/bool/unit values");
        };
        switch (e->kind) {
        case Expr::Integer: case Expr::Boolean: builder.constant(0, e->number); return;
        case Expr::Unit: builder.constant(0, 0); return;
        case Expr::Variable: {
            auto i = env.find(e->text); if (i == env.end()) { unsupported(); return; }
            emit(O::Mov, 0, i->second); return;
        }
        case Expr::Let: {
            const auto& rhs = e->children[0];
            if (!used(e->children[1], e->text) &&
                (rhs->kind == Expr::Lambda || rhs->kind == Expr::Integer || rhs->kind == Expr::Boolean || rhs->kind == Expr::Variable)) {
                child(1); return;
            }
            if (rhs->kind == Expr::Lambda) {
                auto f = std::make_shared<NativeFunction>(); f->label = label(); f->name = e->text;
                f->recursive = e->recursive; f->captured = env; f->functions = functions;
                if (e->recursive) f->captured.erase(e->text);
                auto body = rhs;
                while (body->kind == Expr::Lambda) { f->parameters.push_back(body->text); body = body->children[0]; }
                f->body = body; functions[e->text] = f; env.erase(e->text);
                expression(e->children[1], env, functions, next); return;
            }
            if (rhs->kind == Expr::Variable && !env.contains(rhs->text) && functions.contains(rhs->text)) {
                functions[e->text] = functions.at(rhs->text); env.erase(e->text);
                expression(e->children[1], env, functions, next); return;
            }
            child(0);
            if (next >= 32) throw Error(e->loc, "native backend scalar-local register limit exceeded");
            auto r = static_cast<std::uint8_t>(next++); emit(O::Mov, r, 0); env[e->text] = r; functions.erase(e->text);
            expression(e->children[1], env, functions, next); return;
        }
        case Expr::Apply: {
            std::vector<Tree> arguments; auto head = e;
            while (head->kind == Expr::Apply) { arguments.push_back(head->children[1]); head = head->children[0]; }
            std::reverse(arguments.begin(), arguments.end());
            if (head->kind != Expr::Variable || env.contains(head->text) || !functions.contains(head->text)) { unsupported(); return; }
            auto f = functions.at(head->text);
            if (arguments.size() != (f->print ? 1 : f->parameters.size())) { unsupported(); return; }
            if (f->print) { expression(arguments[0], env, functions, next); emit(O::Print, 0); builder.constant(0, 0); return; }
            if (!f->queued) { f->queued = true; pending.push_back(f); }
            // Arguments and saved registers share MetaIL's explicit value stack.
            for (unsigned r = 2; r < 32; ++r) emit(O::Push, static_cast<std::uint8_t>(r));
            for (const auto& arg : arguments) { expression(arg, env, functions, next); emit(O::Push, 0); }
            builder.jump(O::Call, f->label);
            for (unsigned r = 32; r-- > 2;) emit(O::Pop, static_cast<std::uint8_t>(r));
            return;
        }
        case Expr::If: {
            auto no = label(), end = label(); child(0); builder.jump(O::Jz, no, 0);
            child(1); builder.jump(O::Jmp, end); builder.label(no); child(2); builder.label(end); return;
        }
        case Expr::Sequence: child(0); child(1); return;
        case Expr::Unary:
            child(0); builder.constant(1, 0);
            if (e->text == "not") emit(O::Eq, 0, 0, 1); else emit(O::Sub, 0, 1, 0);
            return;
        case Expr::Binary: {
            if (e->text == "^") { unsupported(); return; }
            if (e->text == "&&" || e->text == "||") {
                auto end = label(); child(0);
                builder.jump(e->text == "&&" ? O::Jz : O::Jnz, end, 0); child(1); builder.label(end); return;
            }
            child(0); emit(O::Push, 0); child(1); emit(O::Mov, 1, 0); emit(O::Pop, 0);
            if (e->text == "+") emit(O::Add, 0, 0, 1);
            else if (e->text == "-") emit(O::Sub, 0, 0, 1);
            else if (e->text == "*") emit(O::Mul, 0, 0, 1);
            else if (e->text == "/") emit(O::Div, 0, 0, 1);
            else if (e->text == "mod") emit(O::Mod, 0, 0, 1);
            else if (e->text == "=" || e->text == "<>") emit(O::Eq, 0, 0, 1);
            else if (e->text == "<" || e->text == ">=") emit(O::Lt, 0, 0, 1);
            else emit(O::Lt, 0, 1, 0);
            if (e->text == "<>" || e->text == ">=" || e->text == "<=") {
                builder.constant(1, 0); emit(O::Eq, 0, 0, 1);
            }
            return;
        }
        default: unsupported(); return;
        }
    }
public:
    explicit Native(const Options& o) : options(o) {}
    metail::Program build(const Tree& tree) {
        Functions functions; auto print = std::make_shared<NativeFunction>(); print->print = true;
        functions["println_int"] = print;
        expression(tree, {}, functions); builder.halt(0);
        for (std::size_t n = 0; n < pending.size(); ++n) {
            auto f = pending[n]; builder.label(f->label);
            auto env = f->captured; auto funcs = f->functions;
            if (f->recursive) funcs[f->name] = f;
            unsigned next = 2; for (const auto& [name, r] : env) { (void)name; next = std::max(next, unsigned(r) + 1); }
            std::vector<std::uint8_t> params;
            for (const auto& name : f->parameters) {
                if (next >= 32) throw Error(f->body->loc, "native backend scalar-local register limit exceeded");
                params.push_back(static_cast<std::uint8_t>(next)); env[name] = static_cast<std::uint8_t>(next++); funcs.erase(name);
            }
            emit(metail::Op::Pop, 1); // temporarily remove the return address
            for (auto r = params.rbegin(); r != params.rend(); ++r) emit(metail::Op::Pop, *r);
            emit(metail::Op::Push, 1);
            expression(f->body, env, funcs, next); emit(metail::Op::Ret);
        }
        auto result = builder.finish();
        if (!result) throw Error(tree->loc, result.diagnostics.front().message);
        if (result.program.size() > options.max_values) throw Error(tree->loc, "native instruction limit exceeded");
        return std::move(result.program);
    }
};
}
std::vector<std::uint8_t> executable(std::string_view source, Options options) {
    auto program = compile(source, "<native>", options);
    if (program.type != "int" && program.type != "bool" && program.type != "unit")
        throw Error({"<native>"}, "native program result must be int, bool or unit");
    auto il = Native(options).build(program.impl->tree);
    metachain::Options config; config.vm.max_steps = options.max_steps;
    config.vm.max_stack_values = options.max_values;
    metachain::Toolchain chain(config);
    if (options.optimize) {
        auto optimized = metail::optimize(std::move(il));
        if (!optimized) throw Error({"<native>"}, optimized.diagnostics.front().message);
        il = std::move(optimized.program);
    }
    auto image = chain.executable(il);
    if (!image) throw Error({"<native>"}, image.diagnostics.front().message);
    return std::move(*image);
}
} // namespace gateml
