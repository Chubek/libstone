#include "Internal.hpp"

namespace gateml {
namespace {
// Symbolic operator names are exposed as ordinary curried builtin functions so
// `(+)` can be passed as a value. They are deliberately not keywords: the
// infix syntax in the grammar is independent of these bindings.
const std::vector<std::string> operators = {
    "+", "-", "*", "/", "mod", "=", "<>", "<", ">", "<=", ">=", "&&", "||", "^"
};
const std::vector<std::pair<std::string, unsigned>> primitives = {
    {"Some", 1}, {"Ok", 1}, {"Error", 1}, {"fst", 1}, {"snd", 1},
    {"print_int", 1}, {"println_int", 1}, {"print_string", 1}, {"print_endline", 1},
    {"Int.to_string", 1}, {"Int.of_string", 1}, {"Int.logand", 2},
    {"Int.logor", 2}, {"Int.logxor", 2}, {"Int.shift_left", 2}, {"Int.shift_right", 2},
    {"String.length", 1}, {"String.get", 2}, {"String.sub", 3}, {"String.compare", 2},
    {"String.of_int", 1}, {"failwith", 1}
};
}
Env builtins() {
    Env env;
    env["None"] = value(Value::Kind::None);
    for (const auto& primitive : primitives) {
        Closure c{}; c.builtin = primitive.first; c.arity = primitive.second;
        env.emplace(primitive.first, function(std::move(c)));
    }
    for (const auto& name : operators) {
        Closure c{}; c.builtin = "#operator:" + name; c.arity = 2;
        env.emplace(name, function(std::move(c)));
    }
    Closure negate{}; negate.builtin = "#unary:not"; negate.arity = 1;
    env.emplace("#not", function(std::move(negate)));
    return env;
}
TypeEnv builtin_types(unsigned& next) {
    TypeEnv env;
    auto variable = [&] { auto t = type(""); t->id = next++; return t; };
    auto arrow = [](Ty a, Ty b) { return type("->", {a, b}); };
    auto integer = type("int"), boolean = type("bool"), string = type("string"), unit = type("unit");
    auto add = [&](std::string name, std::vector<Ty> args, Ty result) {
        for (auto i = args.rbegin(); i != args.rend(); ++i) result = arrow(*i, result);
        std::set<unsigned> vars;
        std::function<void(Ty)> collect = [&](Ty t) {
            if (t->name.empty()) vars.insert(t->id);
            for (auto a : t->args) collect(a);
        };
        collect(result); env[std::move(name)] = {vars, result};
    };
    auto a = variable(), b = variable();
    add("None", {}, type("option", {a})); add("Some", {a}, type("option", {a}));
    add("Ok", {a}, type("result", {a, b})); add("Error", {b}, type("result", {a, b}));
    add("fst", {type("pair", {a, b})}, a); add("snd", {type("pair", {a, b})}, b);
    add("print_int", {integer}, unit); add("println_int", {integer}, unit);
    add("print_string", {string}, unit); add("print_endline", {string}, unit);
    add("failwith", {string}, a);
    add("Int.to_string", {integer}, string); add("String.of_int", {integer}, string);
    add("Int.of_string", {string}, type("option", {integer}));
    for (auto n : {"Int.logand", "Int.logor", "Int.logxor", "Int.shift_left", "Int.shift_right"})
        add(n, {integer, integer}, integer);
    add("String.length", {string}, integer); add("String.get", {string, integer}, string);
    add("String.sub", {string, integer, integer}, string); add("String.compare", {string, string}, integer);
    for (const auto& name : operators) {
        if (name == "&&" || name == "||") add(name, {boolean, boolean}, boolean);
        else if (name == "=" || name == "<>") { auto a = variable(); add(name, {a, a}, type("bool")); }
        else if (name == "^") add(name, {type("string"), type("string")}, type("string"));
        else if (name == "<" || name == ">" || name == "<=" || name == ">=")
            add(name, {integer, integer}, type("bool"));
        else add(name, {integer, integer}, integer);
    }
    add("#not", {boolean}, boolean);
    return env;
}
} // namespace gateml
