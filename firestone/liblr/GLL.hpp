#pragma once

#include "EBNF.hpp"

#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace firestone::lr {
namespace detail {

enum class TopDownArcKind { epsilon, match, call, return_ };

struct TopDownArc {
    TopDownArcKind kind;
    std::size_t target; // next slot or, for a call, the return address
    Symbol symbol;     // terminal to match or nonterminal to call
};

// Descriptor worklist with a graph-structured stack factored by
// (callee, start position), instead of by (return address, start position).
// Each call node records its callers and its known returns. A new edge
// replays earlier returns, and a new return resumes all previous callers.
// Descriptors are deduplicated by (slot, call node, position).
//
// Witnesses are optional and record only the first proof of each fact.
// Every proof points to already constructed proofs, even for nullable cycles.
template<class Network>
class TopDownWorklist {
public:
    TopDownWorklist(const Network& network, const std::vector<Symbol>& tokens, bool witness)
        : network_(network), tokens_(tokens), witness_(witness) {}

    bool run(Symbol start) {
        const auto root = enter(start, 0);
        while (!agenda_.empty()) {
            const auto descriptor = agenda_.front();
            agenda_.pop_front();
            network_.step(descriptor.slot, [&](const TopDownArc& arc) {
                switch (arc.kind) {
                case TopDownArcKind::epsilon:
                    queue(arc.target, descriptor.call, descriptor.position, descriptor.prefix);
                    break;
                case TopDownArcKind::match:
                    if (descriptor.position < tokens_.size() &&
                        tokens_[descriptor.position] == arc.symbol) {
                        const auto end = descriptor.position + 1;
                        if (mark(arc.target, descriptor.call, end)) {
                            const auto token = addNode(arc.symbol, descriptor.position, end, 0);
                            agenda_.push_back({arc.target, descriptor.call, end,
                                extend(descriptor.prefix, token)});
                        }
                    }
                    break;
                case TopDownArcKind::call:
                    call(arc, descriptor);
                    break;
                case TopDownArcKind::return_:
                    complete(descriptor);
                    break;
                }
            });
            const auto accepted = calls_[root].returns.find(tokens_.size());
            if (accepted != calls_[root].returns.end()) {
                result_ = accepted->second;
                return true;
            }
        }
        return false;
    }

    ParseNode tree() const {
        ParseNode root{};
        std::vector<std::pair<std::size_t, ParseNode*>> work{{result_, &root}};
        while (!work.empty()) {
            const auto [id, node] = work.back();
            work.pop_back();
            const auto& fact = nodes_[id - 1];
            node->symbol = fact.symbol;
            node->begin = fact.begin;
            node->end = fact.end;
            std::size_t count = 0;
            for (auto p = fact.prefix; p != 0; p = prefixes_[p - 1].previous) ++count;
            node->children.resize(count);
            for (auto p = fact.prefix; p != 0; p = prefixes_[p - 1].previous)
                work.emplace_back(prefixes_[p - 1].node, &node->children[--count]);
        }
        return root;
    }

private:
    struct Descriptor { std::size_t slot, call, position, prefix; };
    struct Call {
        Symbol rule;
        std::size_t begin;
        // (return slot, parent call) -> parent prefix
        std::map<std::pair<std::size_t, std::size_t>, std::size_t> callers;
        // End position -> witness node (0 in recognition mode).
        std::map<std::size_t, std::size_t> returns;
    };
    struct Node { Symbol symbol; std::size_t begin, end, prefix; };
    struct Prefix { std::size_t previous, node; };

    std::size_t addNode(Symbol symbol, std::size_t begin, std::size_t end, std::size_t prefix) {
        if (!witness_) return 0;
        nodes_.push_back({symbol, begin, end, prefix});
        return nodes_.size();
    }

    std::size_t extend(std::size_t prefix, std::size_t child) {
        if (!witness_) return 0;
        prefixes_.push_back({prefix, child});
        return prefixes_.size();
    }

    bool mark(std::size_t slot, std::size_t callId, std::size_t position) {
        return seen_.emplace(slot, callId, position).second;
    }

    void queue(std::size_t slot, std::size_t callId, std::size_t position, std::size_t prefix) {
        if (mark(slot, callId, position)) agenda_.push_back({slot, callId, position, prefix});
    }

    std::size_t enter(Symbol rule, std::size_t position) {
        const auto entry = callIds_.emplace(std::make_pair(rule, position), calls_.size());
        const auto id = entry.first->second;
        if (entry.second) {
            calls_.push_back({rule, position, {}, {}});
            network_.enter(rule, [&](std::size_t slot) { queue(slot, id, position, 0); });
        }
        return id;
    }

    void resume(std::pair<std::size_t, std::size_t> edge, std::size_t prefix,
                std::size_t end, std::size_t proof) {
        const auto [slot, parent] = edge;
        if (mark(slot, parent, end))
            agenda_.push_back({slot, parent, end, extend(prefix, proof)});
    }

    void call(const TopDownArc& arc, const Descriptor& descriptor) {
        auto& callee = calls_[enter(arc.symbol, descriptor.position)];
        const auto edge = std::make_pair(arc.target, descriptor.call);
        if (!callee.callers.emplace(edge, descriptor.prefix).second) return;
        for (const auto& returned : callee.returns)
            resume(edge, descriptor.prefix, returned.first, returned.second);
    }

    void complete(const Descriptor& descriptor) {
        auto& completed = calls_[descriptor.call];
        const auto returned = completed.returns.emplace(descriptor.position, 0);
        if (!returned.second) return;
        const auto proof = addNode(completed.rule, completed.begin, descriptor.position,
            descriptor.prefix);
        returned.first->second = proof;
        for (const auto& caller : completed.callers)
            resume(caller.first, caller.second, descriptor.position, proof);
    }

    const Network& network_;
    const std::vector<Symbol>& tokens_;
    bool witness_;
    std::size_t result_ = 0;
    std::map<std::pair<Symbol, std::size_t>, std::size_t> callIds_;
    std::vector<Call> calls_;
    std::set<std::tuple<std::size_t, std::size_t, std::size_t>> seen_;
    std::deque<Descriptor> agenda_;
    std::vector<Node> nodes_;
    std::vector<Prefix> prefixes_;
};

template<class Network>
bool topDownAccepts(const Network& network, const Grammar& grammar, const std::vector<Symbol>& tokens) {
    validateInput(grammar, tokens);
    TopDownWorklist<Network> work(network, tokens, false);
    return work.run(grammar.start());
}

template<class Network>
std::optional<ParseNode> topDownParse(const Network& network, const Grammar& grammar,
                                     const std::vector<Symbol>& tokens) {
    validateInput(grammar, tokens);
    TopDownWorklist<Network> work(network, tokens, true);
    if (!work.run(grammar.start())) return std::nullopt;
    return work.tree();
}

} // namespace detail

// GLL-style descriptor parser over flattened production slots. Supports all
// CFGs, including indirect left recursion and nullable cycles. Does not build
// an SPPF: ambiguous inputs yield one finite derivation, not all alternatives.
class GLL {
public:
    explicit GLL(Grammar grammar) : grammar_(std::move(grammar)) {
        grammar_.validate();
        network_.entries.resize(grammar_.symbols().size());
        for (const auto& production : grammar_.productions()) {
            network_.entries[production.lhs].push_back(network_.slots.size());
            for (const auto symbol : production.rhs) {
                const auto kind = grammar_.symbols()[symbol].terminal
                    ? detail::TopDownArcKind::match : detail::TopDownArcKind::call;
                network_.slots.push_back({kind, network_.slots.size() + 1, symbol});
            }
            network_.slots.push_back({detail::TopDownArcKind::return_, 0, 0});
        }
    }

    const Grammar& grammar() const noexcept { return grammar_; }

    bool accepts(const std::vector<Symbol>& tokens) const {
        return detail::topDownAccepts(network_, grammar_, tokens);
    }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        return detail::topDownParse(network_, grammar_, tokens);
    }

private:
    struct Network {
        std::vector<std::vector<std::size_t>> entries;
        std::vector<detail::TopDownArc> slots;

        template<class Function>
        void enter(Symbol symbol, Function&& visit) const {
            for (const auto slot : entries[symbol]) visit(slot);
        }

        template<class Function>
        void step(std::size_t slot, Function&& visit) const { visit(slots[slot]); }
    };

    Grammar grammar_;
    Network network_;
};

} // namespace firestone::lr
