#pragma once

#include "EBNF.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace firestone::lr {

class PEGExpression {
public:
    static PEGExpression empty() { return sequence({}); }
    static PEGExpression fail() { return choice({}); }
    static PEGExpression end() { return make(Kind::end); }
    static PEGExpression token(Symbol symbol) { return make(Kind::token, symbol); }
    // Consume exactly one of the listed terminals. An empty set never matches.
    static PEGExpression tokenSet(std::vector<Symbol> symbols) {
        std::sort(symbols.begin(), symbols.end());
        return make(Kind::tokenSet, 0, {}, std::move(symbols));
    }
    static PEGExpression rule(Symbol symbol) { return make(Kind::rule, symbol); }
    static PEGExpression sequence(std::vector<PEGExpression> parts) {
        return make(Kind::sequence, 0, std::move(parts));
    }
    static PEGExpression choice(std::vector<PEGExpression> alternatives) {
        return make(Kind::choice, 0, std::move(alternatives));
    }
    static PEGExpression optional(PEGExpression part) {
        return make(Kind::optional, 0, {std::move(part)});
    }
    static PEGExpression repeat(PEGExpression part) {
        return make(Kind::repeat, 0, {std::move(part)});
    }
    static PEGExpression oneOrMore(PEGExpression part) {
        return make(Kind::oneOrMore, 0, {std::move(part)});
    }
    static PEGExpression andPredicate(PEGExpression part) {
        return make(Kind::andPredicate, 0, {std::move(part)});
    }
    static PEGExpression notPredicate(PEGExpression part) {
        return make(Kind::notPredicate, 0, {std::move(part)});
    }

private:
    friend class PEG;
    enum class Kind {
        end, token, tokenSet, rule, sequence, choice, optional, repeat, oneOrMore, andPredicate, notPredicate
    };
    struct Node {
        Kind kind;
        Symbol symbol;
        std::vector<PEGExpression> children;
        std::vector<Symbol> symbols;
    };

    explicit PEGExpression(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

    static PEGExpression make(Kind kind, Symbol symbol = 0, std::vector<PEGExpression> children = {},
                              std::vector<Symbol> symbols = {}) {
        return PEGExpression(std::make_shared<const Node>(Node{kind, symbol, std::move(children),
                                                               std::move(symbols)}));
    }

    std::shared_ptr<const Node> node_;
};

// Token PEG. CFG alternatives become prioritized choice in insertion order;
// optional overrides replace complete rules with PEG expressions. Rules may
// be supplied entirely by overrides (Grammar provides the symbols and start).
// Construction rejects left-recursive rule cycles, conservatively considering
// all ordered alternatives and treating lookahead as potentially zero-width.
// Successful zero-width repetition bodies are diagnosed at evaluation time.
class PEG {
public:
    using Rules = std::map<Symbol, PEGExpression>;

    explicit PEG(Grammar grammar, Rules overrides = {}) : grammar_(std::move(grammar)) {
        if (!isNonterminal(grammar_.start())) throw std::invalid_argument("invalid PEG start rule");
        std::vector<std::vector<PEGExpression>> alternatives(grammar_.symbols().size());
        for (const auto& production : grammar_.productions()) {
            if (overrides.find(production.lhs) != overrides.end()) continue;
            std::vector<PEGExpression> body;
            for (const auto symbol : production.rhs) {
                body.push_back(isNonterminal(symbol) ? PEGExpression::rule(symbol)
                    : PEGExpression::token(symbol));
            }
            alternatives[production.lhs].push_back(PEGExpression::sequence(std::move(body)));
        }
        for (Symbol rule = 0; rule < alternatives.size(); ++rule)
            if (!alternatives[rule].empty())
                overrides.emplace(rule, PEGExpression::choice(std::move(alternatives[rule])));
        rules_ = std::move(overrides);
        for (Symbol rule = 0; rule < grammar_.symbols().size(); ++rule)
            if (isNonterminal(rule) && rules_.find(rule) == rules_.end())
                throw std::invalid_argument("no PEG expression for rule: " + grammar_.symbols()[rule].name);
        validateRules();
        rejectLeftRecursion();
    }

    const Grammar& grammar() const noexcept { return grammar_; }

    std::optional<std::size_t> matchPrefix(const std::vector<Symbol>& tokens) const {
        return matchPrefixAt(tokens, 0);
    }

    // Ordered PEG match at begin; the end and tree spans are absolute offsets.
    std::optional<std::size_t> matchPrefixAt(const std::vector<Symbol>& tokens, std::size_t begin) const {
        validatePrefix(tokens, begin);
        return matchPrefixUnchecked(tokens, begin);
    }

    std::optional<ParseNode> parsePrefix(const std::vector<Symbol>& tokens,
                                         std::size_t begin = 0) const {
        validatePrefix(tokens, begin);
        Evaluation evaluation(*this, tokens, true);
        const auto match = evaluation.matchRule(grammar_.start(), begin);
        if (!match) return std::nullopt;
        return evaluation.tree(match->node);
    }

    bool accepts(const std::vector<Symbol>& tokens) const {
        return matchPrefix(tokens) == tokens.size();
    }

    std::optional<ParseNode> parse(const std::vector<Symbol>& tokens) const {
        validateInput(grammar_, tokens);
        Evaluation evaluation(*this, tokens, true);
        const auto match = evaluation.matchRule(grammar_.start(), 0);
        if (!match || match->end != tokens.size()) return std::nullopt;
        return evaluation.tree(match->node);
    }

private:
    friend class LRDiagBytePEG;
    using Kind = PEGExpression::Kind;

    // The byte PEG supplies a view whose every element is a valid terminal.
    template<class Input>
    std::optional<std::size_t> matchPrefixUnchecked(const Input& tokens, std::size_t begin,
                                                    std::size_t* remaining = nullptr) const {
        Evaluation evaluation(*this, tokens, false, remaining);
        const auto match = evaluation.matchRule(grammar_.start(), begin);
        if (!match) return std::nullopt;
        return match->end;
    }

    void validatePrefix(const std::vector<Symbol>& tokens, std::size_t begin) const {
        if (begin > tokens.size()) throw std::out_of_range("PEG position outside input");
        validateInput(grammar_, tokens);
    }

    template<class Input>
    class Evaluation {
    public:
        struct Match { std::size_t end, node; };

        Evaluation(const PEG& peg, const Input& tokens, bool witness, std::size_t* remaining = nullptr)
            : peg_(peg), tokens_(tokens), witness_(witness), remaining_(remaining),
              memo_(peg.grammar_.symbols().size()) {}

        std::optional<Match> matchRule(Symbol rule, std::size_t begin) {
            auto pos = begin;
            bool matched = false;
            std::vector<Frame> work;
            std::vector<std::size_t> children;
            const auto pushExpression = [&](const PEGExpression& expr) {
                work.push_back({expr.node_.get(), nullptr, 0, pos, children.size()});
            };
            const auto pushRule = [&](Symbol symbol) {
                const auto entry = memo_[symbol].try_emplace(pos);
                auto& memo = entry.first->second;
                if (entry.second) {
                    work.push_back({nullptr, &memo, symbol, pos, children.size()});
                    pushExpression(peg_.rules_.at(symbol));
                } else {
                    if (memo.active)
                        throw std::invalid_argument("PEG left recursion in rule " +
                            peg_.grammar_.symbols()[symbol].name + " at token " + std::to_string(pos));
                    matched = memo.match.has_value();
                    if (matched) {
                        pos = memo.match->end;
                        if (witness_) children.push_back(memo.match->node);
                    }
                }
            };
            pushRule(rule);
            while (!work.empty()) {
                if (remaining_) {
                    if (*remaining_ == 0) throw std::length_error("PEG evaluation work limit exceeded");
                    --*remaining_;
                }
                auto& frame = work.back();
                const auto restore = [&] {
                    pos = frame.begin;
                    children.resize(frame.children);
                };
                const auto complete = [&](bool success) {
                    if (!success) restore();
                    matched = success;
                    work.pop_back();
                };
                if (frame.memo) {
                    // A successful rule replaces the proofs of its body with
                    // one memoized proof. A failed body has already rolled back.
                    if (matched) {
                        if (witness_) {
                            const auto proof = addProof(frame.rule, frame.begin, pos,
                                std::vector<std::size_t>(children.begin() + frame.children, children.end()));
                            children.resize(frame.children);
                            children.push_back(proof);
                            frame.memo->match = Match{pos, proof};
                        } else {
                            frame.memo->match = Match{pos, 0};
                        }
                    }
                    frame.memo->active = false;
                    work.pop_back();
                    continue;
                }

                const auto& expr = *frame.expr;
                switch (expr.kind) {
                case Kind::end:
                    complete(pos == tokens_.size());
                    break;
                case Kind::token:
                case Kind::tokenSet:
                    if (pos == tokens_.size()) {
                        complete(false);
                        break;
                    }
                    if (const auto symbol = tokens_[pos];
                        expr.kind == Kind::token ? symbol == expr.symbol :
                            std::binary_search(expr.symbols.begin(), expr.symbols.end(), symbol)) {
                        if (witness_) children.push_back(addProof(symbol, pos, pos + 1, {}));
                        ++pos;
                        complete(true);
                    } else {
                        complete(false);
                    }
                    break;
                case Kind::rule:
                    if (frame.next++ == 0) pushRule(expr.symbol);
                    else complete(matched);
                    break;
                case Kind::sequence:
                    if (frame.next != 0 && !matched) complete(false);
                    else if (frame.next == expr.children.size()) complete(true);
                    else pushExpression(expr.children[frame.next++]);
                    break;
                case Kind::choice:
                    if (frame.next != 0 && matched) complete(true);
                    else if (frame.next == expr.children.size()) complete(false);
                    else pushExpression(expr.children[frame.next++]);
                    break;
                case Kind::optional:
                    if (frame.next++ == 0) pushExpression(expr.children.front());
                    else complete(true);
                    break;
                case Kind::repeat:
                case Kind::oneOrMore:
                    if (frame.next != 0 && !matched) {
                        complete(frame.next > 1 || expr.kind == Kind::repeat);
                    } else {
                        if (frame.next != 0 && frame.last == pos)
                            throw std::invalid_argument("PEG repetition matched without consuming input");
                        frame.last = pos;
                        ++frame.next;
                        pushExpression(expr.children.front());
                    }
                    break;
                case Kind::andPredicate:
                case Kind::notPredicate:
                    if (frame.next++ == 0) {
                        pushExpression(expr.children.front());
                    } else {
                        restore();
                        complete(expr.kind == Kind::andPredicate ? matched : !matched);
                    }
                    break;
                }
            }
            return memo_[rule].at(begin).match;
        }

        ParseNode tree(std::size_t id) const {
            ParseNode root{};
            std::vector<std::pair<std::size_t, ParseNode*>> work{{id, &root}};
            while (!work.empty()) {
                const auto [proofId, node] = work.back();
                work.pop_back();
                const auto& proof = proofs_[proofId];
                node->symbol = proof.symbol;
                node->begin = proof.begin;
                node->end = proof.end;
                node->children.resize(proof.children.size());
                for (std::size_t i = 0; i < proof.children.size(); ++i)
                    work.emplace_back(proof.children[i], &node->children[i]);
            }
            return root;
        }

    private:
        struct Memo { bool active = true; std::optional<Match> match; };
        struct Proof { Symbol symbol; std::size_t begin, end; std::vector<std::size_t> children; };
        // A frame stores the rollback point for an expression or the output
        // boundary for a rule. Rule frames use stable pointers into memo_.
        struct Frame {
            const PEGExpression::Node* expr;
            Memo* memo;
            Symbol rule;
            std::size_t begin, children;
            std::size_t next = 0, last = 0;
        };

        std::size_t addProof(Symbol symbol, std::size_t begin, std::size_t end,
                             std::vector<std::size_t> children) {
            proofs_.push_back({symbol, begin, end, std::move(children)});
            return proofs_.size() - 1;
        }

        const PEG& peg_;
        const Input& tokens_;
        bool witness_;
        std::size_t* remaining_;
        std::vector<std::map<std::size_t, Memo>> memo_;
        std::vector<Proof> proofs_;
    };

    bool isNonterminal(Symbol symbol) const {
        return symbol < grammar_.symbols().size() && !grammar_.symbols()[symbol].terminal;
    }

    bool isInputTerminal(Symbol symbol) const {
        return symbol < grammar_.symbols().size() && symbol != grammar_.eof() &&
            grammar_.symbols()[symbol].terminal;
    }

    void validateRules() const {
        std::set<const PEGExpression::Node*> seen;
        std::vector<const PEGExpression::Node*> work;
        for (const auto& entry : rules_) {
            if (!isNonterminal(entry.first)) throw std::invalid_argument("invalid PEG rule symbol");
            work.push_back(entry.second.node_.get());
        }
        while (!work.empty()) {
            const auto* node = work.back();
            work.pop_back();
            if (node == nullptr) throw std::invalid_argument("missing PEG expression");
            if (!seen.insert(node).second) continue;
            if (node->kind == Kind::token && !isInputTerminal(node->symbol))
                throw std::invalid_argument("invalid PEG token symbol");
            for (const auto symbol : node->symbols)
                if (!isInputTerminal(symbol)) throw std::invalid_argument("invalid PEG token set symbol");
            if (node->kind == Kind::rule && rules_.find(node->symbol) == rules_.end())
                throw std::invalid_argument("invalid PEG rule reference");
            for (const auto& child : node->children) work.push_back(child.node_.get());
        }
    }

    void rejectLeftRecursion() const {
        using Node = PEGExpression::Node;
        struct Frame {
            const Node* node;
            std::size_t next = 0;
            bool nullable;
        };
        const auto frameFor = [](const Node* node) {
            const auto kind = node->kind;
            return Frame{node, 0, kind == Kind::sequence || kind == Kind::end ||
                kind == Kind::optional || kind == Kind::repeat ||
                kind == Kind::andPredicate || kind == Kind::notPredicate};
        };

        // Visit expressions that can be entered without consuming input.
        // Compute nullability as they finish so a sequence only visits its
        // nullable prefix and the first non-nullable part. A cycle must
        // include a rule reference: expression children alone form a DAG.
        std::map<const Node*, std::optional<bool>> nullable;
        std::vector<Frame> path;
        for (const auto& entry : rules_) {
            const auto* root = entry.second.node_.get();
            if (!nullable.try_emplace(root).second) continue;
            path.push_back(frameFor(root));
            while (!path.empty()) {
                auto& frame = path.back();
                const auto& node = *frame.node;
                const Node* child = nullptr;
                if (node.kind == Kind::rule) {
                    if (frame.next == 0) child = rules_.at(node.symbol).node_.get();
                } else if (frame.next < node.children.size() &&
                           (node.kind != Kind::sequence || frame.nullable)) {
                    child = node.children[frame.next].node_.get();
                }
                if (child == nullptr) {
                    nullable.at(frame.node) = frame.nullable;
                    path.pop_back();
                    continue;
                }
                const auto result = nullable.try_emplace(child);
                if (result.second) {
                    path.push_back(frameFor(child));
                    continue;
                }
                const auto& childNullable = result.first->second;
                if (!childNullable.has_value()) {
                    std::vector<Symbol> cycle;
                    bool inCycle = false;
                    for (const auto& item : path) {
                        if (item.node == child) inCycle = true;
                        if (inCycle && item.node->kind == Kind::rule)
                            cycle.push_back(item.node->symbol);
                    }
                    std::string error = "PEG does not support left recursion: " +
                        grammar_.symbols()[cycle.back()].name;
                    for (const auto rule : cycle) error += " -> " + grammar_.symbols()[rule].name;
                    throw std::invalid_argument(error);
                }
                if (node.kind == Kind::rule || node.kind == Kind::sequence)
                    frame.nullable = *childNullable;
                else if (node.kind == Kind::choice)
                    frame.nullable = frame.nullable || *childNullable;
                // oneOrMore cannot succeed empty: a zero-width body throws.
                ++frame.next;
            }
        }
    }

    Grammar grammar_;
    Rules rules_;
};

} // namespace firestone::lr
