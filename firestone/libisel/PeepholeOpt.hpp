#pragma once
#include "../librewrite/Term.hpp"
namespace firestone::isel {
struct PeepholeRule {
    std::vector<rewrite::Pattern> match;
    std::vector<rewrite::Pattern> replacement;
    std::function<bool(const rewrite::Substitution&)> guard;
};
struct PeepholeResult { std::vector<rewrite::Term> code; std::size_t applications = 0; bool limited = false; };
class PeepholeOpt {
public:
    void add_rule(PeepholeRule rule) {
        if (rule.match.empty()) throw std::invalid_argument("empty peephole");
        std::set<std::string> bound;
        for (const auto& p : rule.match) { auto vars = rewrite::variables(p); bound.insert(vars.begin(), vars.end()); }
        for (const auto& p : rule.replacement) for (const auto& v : rewrite::variables(p))
            if (!bound.count(v)) throw std::invalid_argument("unbound peephole variable");
        rules_.push_back(std::move(rule));
    }
    PeepholeResult run(std::vector<rewrite::Term> code, std::size_t budget = 10000) const {
        for (const auto& t : code) rewrite::validate_term(t);
        PeepholeResult out{std::move(code), 0, false};
        for (;;) {
            bool changed = false;
            for (std::size_t pos = 0; pos < out.code.size() && !changed; ++pos) for (const auto& rule : rules_) {
                if (rule.match.size() > out.code.size() - pos) continue;
                rewrite::Substitution bindings; bool matches = true;
                for (std::size_t i = 0; i < rule.match.size(); ++i)
                    if (!rewrite::match_into(rule.match[i], out.code[pos + i], bindings)) { matches = false; break; }
                if (!matches || (rule.guard && !rule.guard(bindings))) continue;
                std::vector<rewrite::Term> replacement;
                for (const auto& p : rule.replacement) replacement.push_back(rewrite::substitute(p, bindings));
                if (replacement.size() == rule.match.size() && std::equal(replacement.begin(), replacement.end(), out.code.begin() + pos)) continue;
                if (out.applications == budget) { out.limited = true; return out; }
                auto at = out.code.erase(out.code.begin() + pos, out.code.begin() + pos + rule.match.size());
                out.code.insert(at, replacement.begin(), replacement.end()); ++out.applications; changed = true; break;
            }
            if (!changed) return out;
        }
    }
private:
    std::vector<PeepholeRule> rules_;
};
} // namespace firestone::isel
