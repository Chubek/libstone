#include "Internal.hpp"

namespace gateml {
Ty type(std::string name, std::vector<Ty> args) {
    auto t = std::make_shared<Type>(); t->name = std::move(name); t->args = std::move(args); return t;
}
Ty prune(Ty t) { if (t->link) t->link = prune(t->link); return t->link ? t->link : t; }
std::string type_name(Ty t) {
    std::map<unsigned, std::string> names;
    std::size_t work = 0;
    std::function<std::string(Ty)> show = [&](Ty x) -> std::string {
        if (++work > 100'000) throw Error({}, "type display work limit exceeded");
        x = prune(x);
        if (x->name.empty()) {
            auto [it, added] = names.emplace(x->id, "");
            if (added) { auto n = names.size() - 1; it->second = "'" + std::string(1, char('a' + n % 26)) + (n >= 26 ? std::to_string(n / 26) : ""); }
            return it->second;
        }
        if (x->name == "->") return "(" + show(x->args[0]) + " -> " + show(x->args[1]) + ")";
        if (x->name == "pair") return "(" + show(x->args[0]) + " * " + show(x->args[1]) + ")";
        if (x->name == "result") return "(" + show(x->args[0]) + ", " + show(x->args[1]) + ") result";
        if (!x->args.empty()) return show(x->args[0]) + " " + x->name;
        return x->name;
    };
    return show(std::move(t));
}
} // namespace gateml
