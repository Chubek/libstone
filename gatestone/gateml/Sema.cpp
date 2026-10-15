#include "Internal.hpp"

namespace gateml {
namespace {
class Inference {
    unsigned& next;
    const Options& options;
    std::size_t work = 0;
    Location current;
    void tick() {
        if (++work > options.max_values) throw Error(current, "type inference work limit exceeded");
    }
    Ty fresh() { auto t = type(""); t->id = next++; return t; }
    void free(Ty t, std::set<unsigned>& vars) {
        tick();
        t = prune(t); if (t->name.empty()) vars.insert(t->id);
        for (auto a : t->args) free(a, vars);
    }
    void unify(Ty a, Ty b, const Location& loc) {
        tick();
        a = prune(a); b = prune(b); if (a == b) return;
        if (a->name.empty()) {
            std::set<unsigned> vars; free(b, vars);
            if (vars.contains(a->id)) throw Error(loc, "infinite type (occurs check)");
            a->link = b; return;
        }
        if (b->name.empty()) { unify(b, a, loc); return; }
        if (a->name != b->name || a->args.size() != b->args.size())
            throw Error(loc, "type mismatch: " + type_name(a) + " versus " + type_name(b));
        for (std::size_t i = 0; i < a->args.size(); ++i) unify(a->args[i], b->args[i], loc);
    }
    Scheme generalize(Ty t, const TypeEnv& env) {
        std::set<unsigned> vars; free(t, vars);
        for (const auto& [name, s] : env) {
            (void)name; std::set<unsigned> v; free(s.type, v);
            for (auto id : v) if (!s.variables.contains(id)) vars.erase(id);
        }
        return {vars, t};
    }
    Ty instantiate(const Scheme& s) {
        std::map<unsigned, Ty> replacements;
        std::function<Ty(Ty)> copy = [&](Ty t) -> Ty {
            tick();
            t = prune(t);
            if (t->name.empty()) {
                if (!s.variables.contains(t->id)) return t;
                auto& v = replacements[t->id]; if (!v) v = fresh(); return v;
            }
            std::vector<Ty> args; for (auto a : t->args) args.push_back(copy(a));
            return type(t->name, std::move(args));
        };
        return copy(s.type);
    }
    void pattern(const Pattern& p, Ty expected, TypeEnv& env, std::set<std::string>& names) {
        auto bind = [&](const std::string& n, Ty t) {
            if (!names.insert(n).second) throw Error(p.loc, "duplicate pattern variable: " + n);
            env[n] = {{}, t};
        };
        switch (p.kind) {
        case Pattern::Wild: return;
        case Pattern::Bind: bind(p.text, expected); return;
        case Pattern::Integer: unify(expected, type("int"), p.loc); return;
        case Pattern::Boolean: unify(expected, type("bool"), p.loc); return;
        case Pattern::Unit: unify(expected, type("unit"), p.loc); return;
        case Pattern::Nil: unify(expected, type("list", {fresh()}), p.loc); return;
        case Pattern::Cons: {
            auto a = fresh(); unify(expected, type("list", {a}), p.loc);
            pattern(p.children[0], a, env, names); pattern(p.children[1], expected, env, names); return;
        }
        case Pattern::Pair: {
            auto a = fresh(), b = fresh(); unify(expected, type("pair", {a, b}), p.loc);
            pattern(p.children[0], a, env, names); pattern(p.children[1], b, env, names); return;
        }
        case Pattern::None: unify(expected, type("option", {fresh()}), p.loc); return;
        case Pattern::Some: {
            auto a = fresh(); unify(expected, type("option", {a}), p.loc); pattern(p.children[0], a, env, names); return;
        }
        case Pattern::Ok: case Pattern::Error: {
            auto a = fresh(), b = fresh(); unify(expected, type("result", {a, b}), p.loc);
            pattern(p.children[0], p.kind == Pattern::Ok ? a : b, env, names); return;
        }
        }
    }
public:
    Inference(unsigned& next, const Options& o) : next(next), options(o) {}
    Ty expression(const Tree& e, TypeEnv& env, std::size_t depth = 0) {
        current = e->loc; tick();
        if (depth > options.max_depth) throw Error(e->loc, "expression depth limit exceeded");
        auto child = [&](std::size_t n) { return expression(e->children[n], env, depth + 1); };
        auto require = [&](Ty a, Ty b) { unify(a, b, e->loc); };
        switch (e->kind) {
        case Expr::Integer: return type("int"); case Expr::Boolean: return type("bool");
        case Expr::String: return type("string"); case Expr::Unit: return type("unit");
        case Expr::Nil: return type("list", {fresh()});
        case Expr::Variable: {
            auto i = env.find(e->text);
            if (i == env.end()) throw Error(e->loc, "unbound name: " + e->text);
            return instantiate(i->second);
        }
        case Expr::Lambda: {
            auto local = env; auto a = fresh(); local[e->text] = {{}, a};
            return type("->", {a, expression(e->children[0], local, depth + 1)});
        }
        case Expr::Apply: {
            auto f = child(0), a = child(1), b = fresh(); require(f, type("->", {a, b})); return b;
        }
        case Expr::Let: {
            auto local = env; Ty a;
            if (e->recursive) {
                auto self = fresh(); local[e->text] = {{}, self};
                a = expression(e->children[0], local, depth + 1); require(self, a);
            } else a = child(0);
            local[e->text] = generalize(a, env);
            return expression(e->children[1], local, depth + 1);
        }
        case Expr::If: {
            require(child(0), type("bool")); auto a = child(1); require(a, child(2)); return a;
        }
        case Expr::Sequence: require(child(0), type("unit")); return child(1);
        case Expr::Pair: return type("pair", {child(0), child(1)});
        case Expr::Cons: { auto a = child(0), b = child(1); require(b, type("list", {a})); return b; }
        case Expr::Unary: {
            auto t = type(e->text == "not" ? "bool" : "int"); require(child(0), t); return t;
        }
        case Expr::Binary: {
            auto a = child(0), b = child(1); require(a, b);
            if (e->text == "=" || e->text == "<>") return type("bool");
            if (e->text == "&&" || e->text == "||") { require(a, type("bool")); return a; }
            if (e->text == "^") { require(a, type("string")); return a; }
            require(a, type("int"));
            if (e->text == "<" || e->text == ">" || e->text == "<=" || e->text == ">=") return type("bool");
            return a;
        }
        case Expr::Match: {
            auto subject = child(0), result = fresh();
            for (std::size_t i = 0; i < e->patterns.size(); ++i) {
                auto local = env; std::set<std::string> names;
                pattern(e->patterns[i], subject, local, names);
                require(result, expression(e->children[i + 1], local, depth + 1));
            }
            return result;
        }
        }
        throw Error(e->loc, "invalid syntax node");
    }
};
}
Ty infer(const Tree& tree, TypeEnv env, unsigned& next, const Options& options) {
    return Inference(next, options).expression(tree, env);
}
} // namespace gateml
