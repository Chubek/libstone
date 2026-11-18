#pragma once

#include "ParserGenerator.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace firestone::lr {

using ForestNodeId = std::size_t;

struct PackedAlternative {
    std::size_t production; // Index into the grammar's productions().
    std::vector<ForestNodeId> children;
};

struct PackedNode {
    Symbol symbol;
    std::size_t begin, end; // Half-open interval in the public input token vector.
    bool terminal;
    std::vector<PackedAlternative> alternatives; // Empty for terminals; epsilon has an alternative with no children.
};

struct ParseForest {
    ForestNodeId root;
    std::vector<PackedNode> nodes;
    // Contains only nodes reachable from root. Recursive derivations for
    // the same span can form cycles; use visited IDs for graph traversal.
};

namespace detail {

class GLRRun {
public:
    GLRRun(const LRTable& table, const std::vector<Symbol>& input) : table_(table), input_(input) {}

    std::optional<ParseForest> parse() {
        Frontier frontier;
        stackNode(frontier, 0, 0);
        for (std::size_t position = 0; !frontier.empty(); ++position) {
            const auto atEnd = position == input_.size();
            const auto lookahead = atEnd ? table_.grammar().eof() : input_[position];
            close(frontier, position, lookahead);
            Frontier next;
            for (const auto& top : frontier) {
                for (const auto& action : table_.actions(top.first, lookahead)) {
                    if (action.kind == LRActionKind::accept) {
                        // The augmented production has just one edge, from
                        // the accepting top back to the initial stack node.
                        for (const auto& edge : stack_[top.second].edges) {
                            if (edge.predecessor == 0 && forest_[edge.label].symbol == table_.grammar().start()) {
                                return finish(edge.label);
                            }
                        }
                    } else if (action.kind == LRActionKind::shift) {
                        const auto leaf = forestNode(lookahead, position, position + 1);
                        const auto dest = stackNode(next, action.target, position + 1);
                        stack_[dest].edges.insert({top.second, leaf});
                    }
                }
            }
            frontier = std::move(next);
        }
        return std::nullopt;
    }

private:
    using StackId = std::size_t;
    using Frontier = std::map<LRState, StackId>;

    struct StackEdge {
        StackId predecessor;
        ForestNodeId label;

        bool operator<(const StackEdge& other) const {
            return std::tie(predecessor, label) < std::tie(other.predecessor, other.label);
        }
    };

    struct StackNode {
        LRState state;
        std::size_t position;
        std::set<StackEdge> edges;
    };

    struct AlternativeOrder {
        bool operator()(const PackedAlternative& lhs, const PackedAlternative& rhs) const {
            return std::tie(lhs.production, lhs.children) < std::tie(rhs.production, rhs.children);
        }
    };

    struct ForestEntry {
        Symbol symbol;
        std::size_t begin, end;
        std::set<PackedAlternative, AlternativeOrder> alternatives;
    };

    struct Reduction {
        StackId predecessor;
        std::vector<ForestNodeId> children;
    };

    const LRTable& table_;
    const std::vector<Symbol>& input_;
    std::vector<StackNode> stack_;
    std::vector<ForestEntry> forest_;
    std::map<std::tuple<Symbol, std::size_t, std::size_t>, ForestNodeId> forestIds_;

    StackId stackNode(Frontier& frontier, LRState state, std::size_t position) {
        const auto inserted = frontier.emplace(state, stack_.size());
        if (inserted.second) stack_.push_back({state, position, {}});
        return inserted.first->second;
    }

    ForestNodeId forestNode(Symbol symbol, std::size_t begin, std::size_t end) {
        const auto inserted = forestIds_.emplace(std::make_tuple(symbol, begin, end), forest_.size());
        if (inserted.second) forest_.push_back({symbol, begin, end, {}});
        return inserted.first->second;
    }

    std::vector<Reduction> paths(StackId top, const std::vector<Symbol>& rhs) const {
        // Traverse backward exactly rhs.size() edges, even in a cyclic
        // graph. Store the matching paths before adding new graph edges.
        using EdgeIterator = std::set<StackEdge>::const_iterator;
        struct Frame {
            StackId node;
            EdgeIterator current, end;
        };
        auto frame = [&](StackId node) {
            return Frame{node, stack_[node].edges.begin(), stack_[node].edges.end()};
        };
        std::vector<Reduction> result;
        std::vector<ForestNodeId> children(rhs.size());
        std::vector<Frame> frames{frame(top)};
        while (!frames.empty()) {
            auto& f = frames.back();
            const auto depth = frames.size() - 1;
            if (depth == rhs.size()) {
                result.push_back({f.node, children});
                frames.pop_back();
            } else if (f.current == f.end) {
                frames.pop_back();
            } else {
                const auto edge = *f.current++;
                const auto i = rhs.size() - depth - 1;
                if (forest_[edge.label].symbol != rhs[i]) continue;
                children[i] = edge.label;
                frames.push_back(frame(edge.predecessor));
            }
        }
        return result;
    }

    void close(Frontier& frontier, std::size_t position, Symbol lookahead) {
        bool changed;
        do {
            changed = false;
            // A new edge may enable reductions from *other* stack tops. Scan
            // again until no edge was added; alternatives alone don't change
            // stack reachability. The graph has finitely many states, token
            // boundaries and edge labels, even for nullable cycles.
            for (const auto& top : frontier) {
                for (const auto& action : table_.actions(top.first, lookahead)) {
                    if (action.kind != LRActionKind::reduce) continue;
                    const auto& rule = table_.grammar().productions()[action.target];
                    for (auto& path : paths(top.second, rule.rhs)) {
                        const auto& base = stack_[path.predecessor];
                        const auto dest = table_.goTo(base.state, rule.lhs);
                        if (!dest) continue;
                        const auto label = forestNode(rule.lhs, base.position, position);
                        forest_[label].alternatives.insert({action.target, std::move(path.children)});
                        const auto newTop = stackNode(frontier, *dest, position);
                        changed |= stack_[newTop].edges.insert({path.predecessor, label}).second;
                    }
                }
            }
        } while (changed);
    }

    ParseForest finish(ForestNodeId root) const {
        std::vector<bool> seen(forest_.size());
        std::vector<ForestNodeId> work{root};
        seen[root] = true;
        for (std::size_t i = 0; i < work.size(); ++i) {
            for (const auto& alternative : forest_[work[i]].alternatives) {
                for (const auto child : alternative.children) {
                    if (!seen[child]) {
                        seen[child] = true;
                        work.push_back(child);
                    }
                }
            }
        }
        std::vector<ForestNodeId> ids(forest_.size());
        ParseForest result{0, {}};
        result.nodes.reserve(work.size());
        for (ForestNodeId id = 0; id < work.size(); ++id) ids[work[id]] = id;
        for (const auto old : work) {
            const auto& entry = forest_[old];
            PackedNode node{entry.symbol, entry.begin, entry.end, table_.grammar().symbols()[entry.symbol].terminal, {}};
            for (const auto& alternative : entry.alternatives) {
                node.alternatives.push_back(alternative);
                for (auto& child : node.alternatives.back().children) child = ids[child];
            }
            result.nodes.push_back(std::move(node));
        }
        return result;
    }
};

} // namespace detail

class GLRParser {
public:
    explicit GLRParser(const Grammar& grammar, LRMode mode = LRMode::canonicalLR1)
        : GLRParser(generateLRTable(grammar, mode)) {}

    explicit GLRParser(LRTable table) : table_(std::move(table)) {}

    const LRTable& table() const { return table_; }

    // Returns no forest on syntax error. Public input omits the EOF token.
    std::optional<ParseForest> parse(const std::vector<Symbol>& input) const {
        validateInput(table_.grammar(), input);
        return detail::GLRRun(table_, input).parse();
    }

private:
    LRTable table_;
};

} // namespace firestone::lr
