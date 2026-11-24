#pragma once

#include "ParserGenerator.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
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
    ForestNodeId root = 0;
    std::vector<PackedNode> nodes;
    // Contains only nodes reachable from root. Recursive derivations for
    // the same span can form cycles; use visited IDs for graph traversal.

    // Extract one finite derivation, even if earlier alternatives are cyclic.
    // Returns no tree for an unproductive root; invalid IDs throw out_of_range.
    std::optional<ParseNode> tree() const {
        if (root >= nodes.size()) throw std::out_of_range("parse forest root outside nodes");
        struct Candidate {
            ForestNodeId parent;
            const PackedAlternative* alternative;
            std::size_t pending;
        };
        std::vector<Candidate> candidates;
        std::vector<std::vector<std::size_t>> users(nodes.size());
        std::vector<const PackedAlternative*> selected(nodes.size());
        std::vector<bool> finite(nodes.size());
        std::vector<ForestNodeId> ready;
        const auto mark = [&](ForestNodeId id, const PackedAlternative* alternative) {
            if (finite[id]) return;
            finite[id] = true;
            selected[id] = alternative;
            ready.push_back(id);
        };
        for (ForestNodeId id = 0; id < nodes.size(); ++id) {
            const auto& node = nodes[id];
            if (node.terminal) {
                mark(id, nullptr);
                continue;
            }
            for (const auto& alternative : node.alternatives) {
                if (alternative.children.empty()) {
                    mark(id, &alternative);
                    continue;
                }
                for (const auto child : alternative.children) users.at(child).push_back(candidates.size());
                candidates.push_back({id, &alternative, alternative.children.size()});
            }
        }
        // An alternative is ready only after every child already has a proof.
        // Duplicate child IDs contribute separate edges and are counted twice.
        for (std::size_t i = 0; !finite[root] && i < ready.size(); ++i) {
            for (const auto user : users[ready[i]]) {
                auto& candidate = candidates[user];
                if (--candidate.pending == 0) mark(candidate.parent, candidate.alternative);
            }
        }
        if (!finite[root]) return std::nullopt;

        ParseNode result{};
        std::vector<std::pair<ForestNodeId, ParseNode*>> work{{root, &result}};
        while (!work.empty()) {
            const auto [id, node] = work.back();
            work.pop_back();
            const auto& packed = nodes[id];
            node->symbol = packed.symbol;
            node->begin = packed.begin;
            node->end = packed.end;
            if (const auto* alternative = selected[id]) {
                node->children.resize(alternative->children.size());
                for (std::size_t i = 0; i < node->children.size(); ++i)
                    work.emplace_back(alternative->children[i], &node->children[i]);
            }
        }
        return result;
    }
};

namespace detail {

class GLRRun {
public:
    GLRRun(const LRTable& table, const std::vector<Symbol>& input, bool pack)
        : table_(table), input_(input), pack_(pack) {}

    std::optional<ParseForest> parse() {
        const auto root = run();
        if (!root) return std::nullopt;
        return finish(*root);
    }

    bool accepts() { return run().has_value(); }

private:
    std::optional<ForestNodeId> run() {
        Frontier frontier;
        stackNode(frontier, 0, 0);
        for (std::size_t position = 0; !frontier.empty(); ++position) {
            const auto atEnd = position == input_.size();
            const auto lookahead = atEnd ? table_.grammar().eof() : input_[position];
            close(frontier, position, lookahead);
            Frontier next;
            for (const auto& top : frontier) {
                for (const auto& action : table_.actions(top.first, lookahead)) {
                    if (atEnd && action.kind == LRActionKind::accept) {
                        // The augmented production has just one edge, from
                        // the accepting top back to the initial stack node.
                        for (const auto& edge : stack_[top.second].edges) {
                            if (edge.predecessor == 0 && forest_[edge.label].symbol == table_.grammar().start()) {
                                return edge.label;
                            }
                        }
                    } else if (!atEnd && action.kind == LRActionKind::shift) {
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
    bool pack_;
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
        std::vector<ForestNodeId> children(pack_ ? rhs.size() : 0);
        std::vector<Frame> frames{frame(top)};
        // For recognition, prefixes reaching the same node at the same
        // depth have identical remaining RHS checks.
        std::set<std::pair<std::size_t, StackId>> seen;
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
                if (pack_) children[i] = edge.label;
                else if (!seen.emplace(depth + 1, edge.predecessor).second) continue;
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
                        if (pack_)
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

    bool accepts(const std::vector<Symbol>& input) const {
        validateInput(table_.grammar(), input);
        return detail::GLRRun(table_, input, false).accepts();
    }

    // Returns no forest on syntax error. Public input omits the EOF token.
    std::optional<ParseForest> parse(const std::vector<Symbol>& input) const {
        validateInput(table_.grammar(), input);
        return detail::GLRRun(table_, input, true).parse();
    }

private:
    LRTable table_;
};

} // namespace firestone::lr
