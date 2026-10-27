#pragma once
#include "../librewrite/Rewrite.hpp"
namespace firestone::isel {
// Ordered target macro rules use the same terms/substitutions as librewrite.
class MacroExpansion {
public:
    void add_rule(rewrite::Rule rule) { engine_.add_rule(std::move(rule)); }
    auto expand(const rewrite::Term& term, std::size_t iterations = 100,
                std::size_t applications = 10000) const { return engine_.run(term, iterations, applications); }
private:
    rewrite::Rewriter engine_;
};
} // namespace firestone::isel
