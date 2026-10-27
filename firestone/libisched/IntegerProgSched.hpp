#pragma once
#include "Common.hpp"
namespace firestone::isched {
struct TimeVariable { NodeId operation; Cycle cycle; };
enum class Relation { Equal, LessEqual, GreaterEqual };
struct IntegerConstraint {
    std::vector<std::pair<std::size_t, std::int64_t>> terms;
    Relation relation = Relation::Equal;
    std::int64_t rhs = 0;
};
struct SchedulingILP {
    std::vector<TimeVariable> binary_variables;
    std::vector<IntegerConstraint> constraints;
    Cycle horizon = 0;
};
struct IntegerProgSched {
    // A binary variable x(i,t) means operation i starts at cycle t. Minimizing
    // makespan is a sequence of bounded feasibility problems, not an LP relaxation.
    SchedulingILP formulate(const Graph& g, const Machine& m, Cycle horizon,
                            std::size_t max_variables = 1000000) const {
        validate(g, m); SchedulingILP model; model.horizon = horizon;
        auto integer = [](std::size_t value) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::overflow_error("ILP coefficient overflow");
            return static_cast<std::int64_t>(value);
        };
        std::vector<std::vector<std::size_t>> variables(g.size());
        std::map<Cycle, IntegerConstraint> issue;
        std::map<std::pair<Cycle, std::string>, IntegerConstraint> resources;
        for (NodeId id = 0; id < g.size(); ++id) {
            const auto& op = g.node(id); IntegerConstraint once; once.rhs = 1;
            if (op.latency <= horizon) for (Cycle t = 0; t <= horizon - op.latency; ++t) {
                if (model.binary_variables.size() >= max_variables) throw std::length_error("ILP variable bound exceeded");
                auto variable = model.binary_variables.size(); model.binary_variables.push_back({id, t});
                variables[id].push_back(variable); once.terms.emplace_back(variable, 1);
                auto& width = issue[t]; width.relation = Relation::LessEqual; width.rhs = integer(m.issue_width);
                width.terms.emplace_back(variable, 1);
                for (Cycle offset = 0; offset < op.occupancy; ++offset) {
                    auto& capacity = resources[{checked_add(t, offset), op.resource}];
                    capacity.relation = Relation::LessEqual; capacity.rhs = integer(m.resources.at(op.resource));
                    capacity.terms.emplace_back(variable, integer(op.units));
                }
            }
            model.constraints.push_back(std::move(once));
        }
        for (auto& row : issue) model.constraints.push_back(std::move(row.second));
        for (auto& row : resources) model.constraints.push_back(std::move(row.second));
        for (const auto& d : g.dependencies()) {
            IntegerConstraint precedence; precedence.relation = Relation::GreaterEqual; precedence.rhs = integer(d.delay);
            for (auto v : variables[d.to]) precedence.terms.emplace_back(v, integer(model.binary_variables[v].cycle));
            for (auto v : variables[d.from]) precedence.terms.emplace_back(v, -integer(model.binary_variables[v].cycle));
            model.constraints.push_back(std::move(precedence));
        }
        return model;
    }
    Schedule schedule(const Graph& g, const Machine& m = {}, std::size_t budget = 100000) const {
        // The internal enumerator generates time variables lazily and enforces
        // the same integer constraints without allocating the sparse matrix.
        return detail::exact(g, m, budget, true);
    }
};
} // namespace firestone::isched
