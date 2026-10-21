#pragma once

#include "TermRewriting.hpp"
#include <string>
#include <vector>

namespace firestone::trs {

struct StringRule {
    std::string name, lhs, rhs;
    StringRule(std::string name, std::string lhs, std::string rhs)
        : name(std::move(name)), lhs(std::move(lhs)), rhs(std::move(rhs)) {
        if (this->lhs.empty()) throw std::invalid_argument("empty string rule lhs");
    }
};

struct StringRewriteResult {
    std::string value;
    RewriteStop stop = RewriteStop::NormalForm;
    std::size_t applications = 0;
    std::vector<std::string> applied_rules;
};

// One leftmost occurrence is replaced per step; rule insertion order breaks
// ties at the same position. A replacement equal to its lhs is a no-op.
class StringRewriter {
public:
    void add_rule(StringRule rule) {
        if (rule.lhs.empty()) throw std::invalid_argument("empty string rule lhs");
        rules_.push_back(std::move(rule));
    }
    const std::vector<StringRule>& rules() const { return rules_; }
    StringRewriteResult run(std::string input, std::size_t iteration_limit = 100,
                            std::size_t application_limit = 10000) const {
        StringRewriteResult result;
        result.value = std::move(input);
        for (std::size_t pass = 0; pass < iteration_limit; ++pass) {
            const StringRule* selected = nullptr;
            std::size_t position = std::string::npos;
            for (const auto& rule : rules_) {
                if (rule.lhs == rule.rhs) continue;
                const auto found = result.value.find(rule.lhs);
                if (found != std::string::npos && (position == std::string::npos || found < position)) {
                    position = found;
                    selected = &rule;
                }
            }
            if (!selected) return result; // normal form: no rule applies
            if (result.applications == application_limit) {
                result.stop = RewriteStop::ApplicationLimit;
                return result;
            }
            result.value.replace(position, selected->lhs.size(), selected->rhs);
            ++result.applications;
            result.applied_rules.push_back(selected->name);
        }
        // The pass budget ran out. If the budget also blocks the next rewrite,
        // report the application limit; otherwise report the iteration limit.
        result.stop = result.applications == application_limit ? RewriteStop::ApplicationLimit
                                                              : RewriteStop::IterationLimit;
        return result;
    }
private:
    std::vector<StringRule> rules_;
};

} // namespace firestone::trs
