#pragma once
#include "../libir/DataflowGraph.hpp"
#include "../librewrite/Term.hpp"
#include "../libeqsat/Extraction.hpp"
#include <cmath>
#include <numeric>

namespace firestone::isel {
using NodeId = ir::NodeId;
struct Node {
    std::string op;
    std::vector<NodeId> inputs;
    ir::Type type;
    std::optional<ir::Instruction> source;
};
// Append-only DAG. Distinct nodes stay distinct, including equal constants.
class Graph {
public:
    NodeId add(std::string op, std::vector<NodeId> inputs = {}, ir::Type type = {},
               std::optional<ir::Instruction> source = {}) {
        if (op.empty()) throw std::invalid_argument("empty selection operator");
        for (auto id : inputs) node(id);
        nodes_.push_back({std::move(op), std::move(inputs), std::move(type), std::move(source)});
        return nodes_.size() - 1;
    }
    const Node& node(NodeId id) const { return nodes_.at(id); }
    std::size_t size() const { return nodes_.size(); }
    NodeId import(const rewrite::Term& term) {
        rewrite::validate_term(term);
        std::vector<NodeId> children;
        for (const auto& c : term.children) children.push_back(import(c));
        return add(term.op, std::move(children));
    }
private:
    std::vector<Node> nodes_;
};
inline std::string opcode_name(ir::Opcode opcode) {
    static const char* names[] = {"constant", "copy", "add", "sub", "mul", "div", "mod", "and", "or", "xor", "shl", "shr", "neg", "not", "eq", "ne", "lt", "le", "gt", "ge", "phi", "select", "load", "store", "call", "jump", "branch", "return"};
    auto i = static_cast<std::size_t>(opcode);
    if (i >= sizeof(names) / sizeof(*names)) throw std::invalid_argument("invalid opcode");
    return names[i];
}
inline Graph from_dataflow(const ir::DataflowGraph& input) {
    auto errors = input.verify();
    if (!errors.empty()) throw std::invalid_argument(errors.front().message);
    Graph out;
    std::map<NodeId, NodeId> ids;
    for (auto id : input.schedule()) {
        const auto& inst = input.node(id);
        std::vector<NodeId> children;
        for (const auto& operand : inst.operands)
            if (operand.kind == ir::OperandKind::Value) children.push_back(ids.at(operand.value));
        ids[id] = out.add(opcode_name(inst.opcode), children, inst.type, inst);
    }
    return out;
}
inline Graph from_extraction(const eqsat::DAGExtraction& extraction) {
    Graph out;
    if (extraction.root >= extraction.nodes.size()) throw std::invalid_argument("invalid extraction root");
    for (const auto& n : extraction.nodes) out.add(n.op, n.children);
    return out;
}
using Bindings = std::map<std::string, NodeId>;
struct Rule {
    std::string name;
    rewrite::Pattern pattern;
    std::string instruction;
    double cost = 1;
    std::size_t latency = 1;
    std::string resource = "alu";
    // Target legality (types, immediates, overflow semantics) belongs to the caller.
    std::function<bool(const Graph&, NodeId, const Bindings&)> guard;
    std::size_t occupancy = 1;
    std::size_t units = 1;
    // Empty retains lexicographic binding order. Explicit order may repeat inputs.
    std::vector<std::string> operand_order;

};
class Rules {
public:
    std::size_t add(Rule rule) {
        rewrite::validate_term(rule.pattern, true);
        if (rewrite::is_variable(rule.pattern) || rule.name.empty() || rule.instruction.empty() ||
            !std::isfinite(rule.cost) || rule.cost < 0 || rule.latency == 0 || rule.resource.empty() || rule.occupancy == 0 || rule.units == 0)
            throw std::invalid_argument("invalid selection rule");
        if (!rule.operand_order.empty()) {
            const auto variables = rewrite::variables(rule.pattern);
            const std::set<std::string> supplied(rule.operand_order.begin(), rule.operand_order.end());
            if (variables != supplied) throw std::invalid_argument("operand order must cover exactly the pattern bindings");
        }
        rules_.push_back(std::move(rule)); return rules_.size() - 1;
    }
    const Rule& operator[](std::size_t id) const { return rules_.at(id); }
    std::size_t size() const { return rules_.size(); }
private:
    std::vector<Rule> rules_;
};
struct Tile {
    NodeId root = 0;
    std::size_t rule = 0;
    std::set<NodeId> covered;
    Bindings bindings;
};
struct Instruction {
    NodeId result = 0;
    std::string opcode;
    std::vector<NodeId> inputs; // Binding order is lexicographic by variable name.
    std::vector<std::string> input_names;
    std::size_t latency = 1;
    std::string resource = "alu";
    std::vector<NodeId> covered;
    std::size_t rule = 0;
    std::size_t occupancy = 1;
    std::size_t units = 1;
};
enum class Stop { Complete, Infeasible, SearchLimit };
struct Selection {
    std::vector<Instruction> instructions; // Definitions before uses; original graph IDs.
    std::vector<NodeId> roots;
    double cost = 0;
    Stop stop = Stop::Complete;
    bool optimal = false;
    std::size_t explored = 0;
};
struct Problem {
    const Graph& graph;
    const Rules& rules;
    std::vector<NodeId> roots;
    std::set<NodeId> live;
    std::vector<Tile> tiles;
    std::map<NodeId, std::vector<std::size_t>> at_root;
    Problem(const Graph& g, const Rules& r, std::vector<NodeId> outputs) : graph(g), rules(r), roots(std::move(outputs)) {
        std::function<void(NodeId)> mark = [&](NodeId id) {
            const auto& n = graph.node(id);
            if (live.insert(id).second) for (auto child : n.inputs) mark(child);
        };
        for (auto id : roots) mark(id);
        std::map<NodeId, std::set<NodeId>> users;
        for (auto id : live) for (auto child : graph.node(id).inputs) users[child].insert(id);
        for (auto id : live) for (std::size_t ri = 0; ri < rules.size(); ++ri) {
            Tile tile{id, ri, {}, {}};
            std::function<bool(const rewrite::Pattern&, NodeId)> match = [&](const rewrite::Pattern& p, NodeId n) {
                if (rewrite::is_variable(p)) {
                    auto it = tile.bindings.emplace(p.op, n);
                    return it.second || it.first->second == n;
                }
                const auto& node = graph.node(n);
                if (p.op != node.op || p.children.size() != node.inputs.size()) return false;
                tile.covered.insert(n);
                for (std::size_t i = 0; i < p.children.size(); ++i) if (!match(p.children[i], node.inputs[i])) return false;
                return true;
            };
            if (!match(rules[ri].pattern, id)) continue;
            bool legal = true;
            for (const auto& b : tile.bindings) if (tile.covered.count(b.second)) legal = false;
            for (auto n : tile.covered) if (n != id) {
                if (std::find(roots.begin(), roots.end(), n) != roots.end()) legal = false;
                for (auto user : users[n]) if (!tile.covered.count(user)) legal = false;
            }
            if (!legal || (rules[ri].guard && !rules[ri].guard(graph, id, tile.bindings))) continue;
            at_root[id].push_back(tiles.size()); tiles.push_back(std::move(tile));
        }
    }
};
inline Selection materialize(const Problem& p, const std::vector<std::size_t>& chosen) {
    Selection out; out.roots = p.roots;
    std::map<NodeId, std::size_t> owners, roots;
    for (auto i : chosen) {
        const auto& t = p.tiles.at(i);
        if (!roots.emplace(t.root, i).second) throw std::invalid_argument("duplicate selected root");
        for (auto n : t.covered) if (!owners.emplace(n, i).second) throw std::invalid_argument("overlapping instruction tiles");
    }
    if (owners.size() != p.live.size()) throw std::invalid_argument("incomplete instruction cover");
    for (auto n : p.live) if (!owners.count(n)) throw std::invalid_argument("uncovered node");
    for (const auto& pair : roots) {
        const auto& t = p.tiles[pair.second]; const auto& rule = p.rules[t.rule];
        Instruction inst{t.root, rule.instruction, {}, {}, rule.latency, rule.resource,
                         {t.covered.begin(), t.covered.end()}, t.rule, rule.occupancy, rule.units};
        auto names = rule.operand_order;
        if (names.empty()) for (const auto& binding : t.bindings) names.push_back(binding.first);
        for (const auto& name : names) {
            const auto input = t.bindings.at(name);
            if (!roots.count(input)) throw std::invalid_argument("tile input is not a selected result");
            inst.inputs.push_back(input); inst.input_names.push_back(name);
        }
        out.instructions.push_back(std::move(inst)); out.cost += rule.cost;
        if (!std::isfinite(out.cost)) throw std::overflow_error("selection cost overflow");
    }
    return out;
}
namespace detail {
inline Selection failed(const Problem& p, Stop stop = Stop::Infeasible) {
    Selection out; out.roots = p.roots; out.stop = stop; return out;
}
// Tree recurrence; shared boundary values are emitted once. DAG cost estimates
// count uses independently, so optimality is claimed only for forests.
inline Selection dynamic(const Problem& p, bool munch, bool bottom_up) {
    std::map<NodeId, double> costs;
    std::map<NodeId, std::size_t> best;
    const auto infinity = std::numeric_limits<double>::infinity();
    std::function<double(NodeId)> solve = [&](NodeId id) {
        if (costs.count(id)) return costs.at(id);
        double score = infinity; std::size_t size = 0;
        auto candidates = p.at_root.find(id);
        if (candidates != p.at_root.end()) for (auto ti : candidates->second) {
            const auto& t = p.tiles[ti]; double cost = p.rules[t.rule].cost;
            std::set<NodeId> inputs;
            for (const auto& b : t.bindings) inputs.insert(b.second);
            for (auto child : inputs) cost += solve(child);
            if (!std::isfinite(cost)) continue;
            if ((munch && t.covered.size() > size) || ((!munch || t.covered.size() == size) && cost < score)) {
                score = cost; size = t.covered.size(); best[id] = ti;
            }
        }
        costs[id] = score; return score;
    };
    if (bottom_up) for (auto id : p.live) solve(id);
    for (auto id : p.roots) if (!std::isfinite(solve(id))) return failed(p);
    std::set<NodeId> visited; std::vector<std::size_t> chosen;
    std::function<void(NodeId)> emit = [&](NodeId id) {
        if (!visited.insert(id).second) return;
        const auto ti = best.at(id); chosen.push_back(ti);
        for (const auto& b : p.tiles[ti].bindings) emit(b.second);
    };
    for (auto id : p.roots) emit(id);
    auto out = materialize(p, chosen);
    std::map<NodeId, std::size_t> uses;
    for (auto id : p.roots) ++uses[id];
    for (auto id : p.live) for (auto c : p.graph.node(id).inputs) ++uses[c];
    out.optimal = !munch && std::all_of(uses.begin(), uses.end(), [](const auto& item) { return item.second <= 1; });
    return out;
}
// Binary exact cover with branch-and-bound. A node row sums to one.
inline Selection exact(const Problem& p, std::size_t budget) {
    auto best = dynamic(p, false, true);
    double incumbent = best.stop == Stop::Complete ? best.cost : std::numeric_limits<double>::infinity();
    std::map<NodeId, std::vector<std::size_t>> rows;
    for (std::size_t i = 0; i < p.tiles.size(); ++i) for (auto n : p.tiles[i].covered) rows[n].push_back(i);
    std::set<NodeId> covered; std::vector<std::size_t> chosen;
    std::size_t explored = 0; bool limited = false;
    std::function<void(double)> search = [&](double cost) {
        if (explored >= budget) { limited = true; return; }
        ++explored;
        if (covered.size() == p.live.size()) {
            if (cost < incumbent || best.stop != Stop::Complete) { best = materialize(p, chosen); incumbent = cost; }
            return;
        }
        if (cost >= incumbent) return;
        std::vector<std::size_t> options; std::size_t count = ir::invalid_id;
        for (auto n : p.live) if (!covered.count(n)) {
            std::vector<std::size_t> legal;
            for (auto i : rows[n]) {
                bool overlap = false;
                for (auto c : p.tiles[i].covered) if (covered.count(c)) overlap = true;
                if (!overlap) legal.push_back(i);
            }
            if (legal.size() < count) { count = legal.size(); options = std::move(legal); }
        }
        std::stable_sort(options.begin(), options.end(), [&](auto a, auto b) { return p.rules[p.tiles[a].rule].cost < p.rules[p.tiles[b].rule].cost; });
        for (auto i : options) {
            for (auto n : p.tiles[i].covered) covered.insert(n);
            chosen.push_back(i); search(cost + p.rules[p.tiles[i].rule].cost); chosen.pop_back();
            for (auto n : p.tiles[i].covered) covered.erase(n);
            if (limited) return;
        }
    };
    search(0); best.explored = explored; best.optimal = !limited && best.stop == Stop::Complete;
    if (limited) best.stop = Stop::SearchLimit;
    return best;
}
} // namespace detail
} // namespace firestone::isel
