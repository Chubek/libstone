#include "liblr/ATN.hpp"
#include "liblr/GLL.hpp"
#include "liblr/GLR.hpp"
#include "liblr/LLFunctionDriven.hpp"
#include "liblr/LLTableDriven.hpp"
#include "liblr/PEG.hpp"
#include "liblr/ParserCombinator.hpp"
#include "liblr/RecursiveDescent.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace firestone::lr;

#define CHECK(...) do { \
    if (!(__VA_ARGS__)) throw std::runtime_error("lr_top_down.cpp:" + std::to_string(__LINE__) \
        + ": " #__VA_ARGS__); \
} while (false)

template<class Exception = std::invalid_argument, class Function>
void expectError(Function test, const std::string& text) {
    try {
        test();
    } catch (const Exception& error) {
        CHECK(std::string(error.what()).find(text) != std::string::npos);
        return;
    }
    throw std::runtime_error("expected exception containing: " + text);
}

void checkTree(const Grammar& grammar, const std::vector<Symbol>& tokens, const ParseNode& root) {
    CHECK(root.symbol == grammar.start());
    CHECK(root.begin == 0 && root.end == tokens.size());
    std::vector<const ParseNode*> work{&root};
    while (!work.empty()) {
        const auto& node = *work.back();
        work.pop_back();
        CHECK(node.symbol < grammar.symbols().size());
        CHECK(node.begin <= node.end && node.end <= tokens.size());
        if (grammar.symbols()[node.symbol].terminal) {
            CHECK(node.children.empty());
            CHECK(node.end > node.begin && node.end - node.begin == 1);
            CHECK(tokens[node.begin] == node.symbol);
            continue;
        }
        std::vector<Symbol> body;
        auto offset = node.begin;
        for (const auto& child : node.children) {
            CHECK(child.begin == offset);
            offset = child.end;
            body.push_back(child.symbol);
            work.push_back(&child);
        }
        CHECK(offset == node.end);
        CHECK(std::any_of(grammar.productions().begin(), grammar.productions().end(),
            [&](const Production& rule) { return rule.lhs == node.symbol && rule.rhs == body; }));
    }
}

struct Parentheses {
    Grammar grammar;
    Symbol open = grammar.terminal("(");
    Symbol close = grammar.terminal(")");
    Symbol start = grammar.nonterminal("S");

    Parentheses() {
        grammar.setStart(start);
        grammar.addProduction(start, {open, start, close, start});
        grammar.addProduction(start, {});
    }
};

void testLL() {
    const Parentheses g;
    const LLTable table(g.grammar);
    CHECK(table.isLL1());
    CHECK(table.nullable(g.start) && !table.nullable(g.open));
    CHECK(table.first(g.start).count(g.open) == 1);
    CHECK(table.first(g.start).count(g.grammar.eof()) == 0);
    CHECK(table.follow(g.start).count(g.close) == 1);
    CHECK(table.follow(g.start).count(g.grammar.eof()) == 1);
    CHECK(table.lookup(g.start, g.open) == 0);
    CHECK(table.lookup(g.start, g.close) == 1);
    CHECK(table.lookup(g.start, g.grammar.eof()) == 1);

    const LLTableDriven tableParser(table);
    const LLFunctionDriven original(table);
    const auto functionParser = original; // closures do not borrow another parser's state
    for (const std::vector<Symbol>& tokens :
         {std::vector<Symbol>{}, {g.open, g.open, g.close, g.close, g.open, g.close}}) {
        const auto fromTable = tableParser.parse(tokens);
        const auto fromFunctions = functionParser.parse(tokens);
        CHECK(fromTable && fromFunctions);
        checkTree(g.grammar, tokens, *fromTable);
        checkTree(g.grammar, tokens, *fromFunctions);
        if (!tokens.empty()) CHECK(fromTable->children[3].begin == 4);
    }
    for (const std::vector<Symbol>& tokens :
         {std::vector<Symbol>{g.open}, {g.close}, {g.open, g.close, g.close}}) {
        CHECK(!tableParser.parse(tokens));
        CHECK(!functionParser.parse(tokens));
    }

    Grammar grammar;
    const auto token = grammar.terminal("x");
    const auto start = grammar.nonterminal("Start");
    const auto rule = grammar.nonterminal("Rule");
    grammar.setStart(start);
    grammar.addProduction(start, {rule, token});
    grammar.addProduction(rule, {token});
    grammar.addProduction(rule, {});
    grammar.addProduction(rule, {token, token});
    const LLTable conflicts(grammar);
    CHECK(!conflicts.isLL1() && conflicts.leftRecursion().empty());
    CHECK(conflicts.conflicts().size() == 1);
    CHECK(conflicts.conflicts()[0].nonterminal == rule && conflicts.conflicts()[0].lookahead == token);
    CHECK(conflicts.conflicts()[0].productions == std::vector<std::size_t>({1, 2, 3}));
    CHECK(!conflicts.lookup(rule, token));
    expectError([&] { (void)LLTableDriven(conflicts); }, "conflict for Rule on x");
    expectError([&] { (void)LLFunctionDriven(conflicts); }, "productions 1 2 3");

    Grammar cycle;
    const auto a = cycle.nonterminal("A");
    const auto b = cycle.nonterminal("B");
    const auto epsilon = cycle.nonterminal("Empty");
    cycle.setStart(a);
    cycle.addProduction(a, {epsilon, b});
    cycle.addProduction(b, {a});
    cycle.addProduction(epsilon, {});
    const LLTable left(cycle);
    CHECK(left.conflicts().empty());
    CHECK(left.leftRecursion() == std::vector<Symbol>({a, b, a}));
    expectError([&] { (void)LLTableDriven(left); }, "left recursion: A -> B -> A");
    expectError([&] { (void)LLFunctionDriven(left); }, "left recursion");
}

void testRecursiveDescent() {
    const Parentheses g;
    const std::vector<Symbol> tokens{g.open, g.open, g.close, g.close};
    RecursiveDescent cursor(g.grammar, tokens);
    CHECK(cursor.peek(1) == g.open);
    CHECK(!cursor.peek(std::numeric_limits<std::size_t>::max()));
    CHECK(!cursor.attempt([&](auto& p) { return p.consume(g.open) && p.consume(g.close); }));
    CHECK(cursor.position() == 0);
    expectError<std::runtime_error>([&] {
        cursor.attempt([](auto& p) -> bool {
            p.take();
            throw std::runtime_error("test");
        });
    }, "test");
    CHECK(cursor.position() == 0);

    auto parse = [&](auto& self) -> bool {
        return cursor.rule(g.start, [&](auto& p) {
            if (!p.consume(g.open)) return true;
            return self(self) && p.consume(g.close);
        });
    };
    CHECK(parse(parse) && cursor.atEnd());
    CHECK(!cursor.take());
    cursor.restore(0);
    expectError([&] {
        cursor.rule(g.start, [&](auto& p) {
            return p.rule(g.open, [&](auto& q) {
                return q.rule(g.start, [](auto&) { return true; });
            });
        });
    }, "left recursion");
    CHECK(cursor.position() == 0 && parse(parse));
    expectError<std::out_of_range>([&] { cursor.restore(99); }, "outside input");
}

template<class Parser>
void checkGeneralized(const Parser& parser, const std::vector<Symbol>& tokens, bool accepted) {
    CHECK(parser.accepts(tokens) == accepted);
    const auto tree = parser.parse(tokens);
    CHECK(static_cast<bool>(tree) == accepted);
    if (tree) checkTree(parser.grammar(), tokens, *tree);
}

void testGeneralized() {
    Grammar grammar;
    const auto x = grammar.terminal("x");
    const auto y = grammar.terminal("y");
    const auto start = grammar.nonterminal("S");
    const auto a = grammar.nonterminal("A");
    grammar.setStart(start);
    grammar.addProduction(start, {a, y});
    grammar.addProduction(a, {a, a});  // ambiguous, and can recurse without progress
    grammar.addProduction(a, {start, x}); // indirect recursion
    grammar.addProduction(a, {x});
    grammar.addProduction(a, {});

    const GLL gll(grammar);
    const ATN atn(grammar);
    CHECK(!atn.entry(x));
    CHECK(atn.entry(start) && atn.entry(a));
    const auto& branches = atn.states()[*atn.entry(start)];
    CHECK(branches.size() == 1 && branches.front().kind == ATN::ArcKind::epsilon);
    const auto& call = atn.states()[branches.front().target].front();
    CHECK(call.kind == ATN::ArcKind::call && call.symbol == a);
    const auto& match = atn.states()[call.target].front();
    CHECK(match.kind == ATN::ArcKind::match && match.symbol == y);
    CHECK(atn.states()[match.target].front().kind == ATN::ArcKind::return_);

    for (const std::vector<Symbol>& tokens :
         {std::vector<Symbol>{y}, {x, x, x, y}, {y, x, y}, {y, x, y, x, y}}) {
        checkGeneralized(gll, tokens, true);
        checkGeneralized(atn, tokens, true);
    }
    for (const std::vector<Symbol>& tokens :
         {std::vector<Symbol>{}, {x}, {y, y}, {y, x, y, x}}) {
        checkGeneralized(gll, tokens, false);
        checkGeneralized(atn, tokens, false);
    }

    Grammar replay;
    const auto root = replay.nonterminal("Root");
    const auto empty = replay.nonterminal("Empty");
    replay.setStart(root);
    replay.addProduction(root, {empty, empty, empty});
    replay.addProduction(empty, {empty});
    replay.addProduction(empty, {});
    // Later callers at the same offset must see already recorded returns.
    checkGeneralized(GLL(replay), {}, true);
    checkGeneralized(ATN(replay), {}, true);

    Grammar unproductive;
    const auto cycle = unproductive.nonterminal("Cycle");
    unproductive.setStart(cycle);
    unproductive.addProduction(cycle, {cycle});
    checkGeneralized(GLL(unproductive), {}, false);
    checkGeneralized(ATN(unproductive), {}, false);
}

// Independent bottom-up closure oracle for short inputs. It does not use
// parser descriptors, lookahead, or transition networks.
bool cfgAccepts(const Grammar& grammar, const std::vector<Symbol>& tokens) {
    const auto n = tokens.size() + 1;
    std::vector<std::vector<bool>> spans(grammar.symbols().size(), std::vector<bool>(n * n));
    for (std::size_t i = 0; i < tokens.size(); ++i) spans[tokens[i]][i * n + i + 1] = true;
    bool changed;
    do {
        changed = false;
        for (const auto& production : grammar.productions()) {
            for (std::size_t begin = 0; begin < n; ++begin) {
                std::vector<bool> positions(n);
                positions[begin] = true;
                for (const auto symbol : production.rhs) {
                    std::vector<bool> next(n);
                    for (std::size_t i = begin; i < n; ++i)
                        if (positions[i])
                            for (std::size_t j = i; j < n; ++j)
                                next[j] = next[j] || spans[symbol][i * n + j];
                    positions = std::move(next);
                }
                for (std::size_t end = begin; end < n; ++end) {
                    if (positions[end] && !spans[production.lhs][begin * n + end]) {
                        spans[production.lhs][begin * n + end] = true;
                        changed = true;
                    }
                }
            }
        }
    } while (changed);
    return spans[grammar.start()][tokens.size()];
}

void checkGeneralizedLR(const GLRParser& parser, const std::vector<Symbol>& tokens, bool accepted) {
    CHECK(parser.accepts(tokens) == accepted);
    const auto forest = parser.parse(tokens);
    CHECK(static_cast<bool>(forest) == accepted);
    if (!forest) return;
    const auto& root = forest->nodes.at(forest->root);
    CHECK(root.symbol == parser.table().grammar().start());
    CHECK(root.begin == 0 && root.end == tokens.size());
    CHECK(!root.alternatives.empty());
    const auto tree = forest->tree();
    CHECK(tree);
    checkTree(parser.table().grammar(), tokens, *tree);
}

void testGeneralizedAgainstOracle() {
    std::mt19937 rng(0x4180u);
    for (int trial = 0; trial < 60; ++trial) {
        Grammar grammar;
        const auto x = grammar.terminal("x");
        const auto y = grammar.terminal("y");
        const auto a = grammar.nonterminal("A");
        const auto b = grammar.nonterminal("B");
        const auto c = grammar.nonterminal("C");
        const std::vector<Symbol> symbols{x, y, a, b, c};
        grammar.setStart(a);
        for (const auto rule : {a, b, c}) {
            const auto count = 1 + rng() % 4;
            for (std::size_t branch = 0; branch < count; ++branch) {
                std::vector<Symbol> body(rng() % 4);
                for (auto& item : body) item = symbols[rng() % symbols.size()];
                grammar.addProduction(rule, std::move(body));
            }
        }
        const GLL gll(grammar);
        const ATN atn(grammar);
        const GLRParser canonical(grammar);
        const GLRParser lalr(grammar, LRMode::lalr1);
        const GLRParser slr(grammar, LRMode::slr1);
        for (unsigned length = 0; length <= 4; ++length) {
            for (unsigned mask = 0; mask < (1u << length); ++mask) {
                std::vector<Symbol> tokens(length);
                for (unsigned i = 0; i < length; ++i) tokens[i] = mask & (1u << i) ? x : y;
                const bool accepted = cfgAccepts(grammar, tokens);
                checkGeneralized(gll, tokens, accepted);
                checkGeneralized(atn, tokens, accepted);
                checkGeneralizedLR(canonical, tokens, accepted);
                checkGeneralizedLR(lalr, tokens, accepted);
                checkGeneralizedLR(slr, tokens, accepted);
            }
        }
    }
}

void testPEG() {
    using P = PEGExpression;
    Grammar grammar;
    const auto x = grammar.terminal("x");
    const auto y = grammar.terminal("y");
    const auto start = grammar.nonterminal("Start");
    grammar.setStart(start);
    grammar.addProduction(start, {x});
    grammar.addProduction(start, {x, y});
    const PEG ordered(grammar);
    CHECK(ordered.matchPrefix({x, y}) == 1);
    CHECK(!ordered.parse({x, y}));
    const auto first = ordered.parse({x});
    CHECK(first);
    checkTree(grammar, {x}, *first);

    const auto tx = P::token(x);
    const auto ty = P::token(y);
    const PEG fallback(grammar, {{start, P::choice({P::sequence({tx, ty}), tx})}});
    CHECK(fallback.accepts({x, y}));
    CHECK(fallback.accepts({x})); // the failed first sequence consumed x before failing
    CHECK(!fallback.matchPrefix({y}));

    const auto rule = grammar.nonterminal("Test");
    const PEG predicates(grammar, {
        {start, P::sequence({P::andPredicate(P::rule(rule)), P::notPredicate(ty),
            P::rule(rule), P::optional(ty), P::end()})},
        {rule, P::oneOrMore(tx)}
    });
    const auto tree = predicates.parse({x, x, y});
    CHECK(tree && tree->children.size() == 2);
    CHECK(tree->children[0].symbol == rule && tree->children[0].end == 2);
    CHECK(tree->children[0].children.size() == 2 && tree->children[1].symbol == y);
    CHECK(predicates.accepts({x}) && !predicates.accepts({}) && !predicates.accepts({y}));
    CHECK(!predicates.accepts({x, y, y}));

    const PEG greedy(grammar, {{start, P::sequence({P::repeat(tx), tx})}, {rule, P::fail()}});
    CHECK(!greedy.accepts({x, x}));
    const PEG repeatingEmpty(grammar, {{start, P::repeat(P::rule(rule))}, {rule, P::empty()}});
    expectError([&] { (void)repeatingEmpty.accepts({}); }, "without consuming");
    expectError([&] {
        (void)PEG(grammar, {
            {start, P::rule(rule)}, {rule, P::sequence({P::andPredicate(tx), P::rule(start)})}
        });
    }, "left recursion: Start -> Test -> Start");
    expectError([&] {
        (void)PEG(grammar, {{rule, P::notPredicate(P::rule(rule))}});
    }, "left recursion: Test -> Test"); // even a rule unreachable from Start is checked
    expectError([&] {
        (void)PEG(grammar, {{start, P::choice({P::empty(), P::rule(start)})}, {rule, P::fail()}});
    }, "left recursion: Start -> Start"); // ordered alternatives are checked conservatively
    for (const auto& prefix : {P::optional(tx), P::repeat(tx), P::end()}) {
        expectError([&] {
            (void)PEG(grammar, {
                {start, P::sequence({prefix, P::rule(start)})}, {rule, P::fail()}
            });
        }, "left recursion: Start -> Start");
    }

    auto nullable = grammar;
    nullable.addProduction(rule, {});
    nullable.addProduction(start, {rule, start});
    expectError([&] { (void)PEG(nullable); }, "left recursion: Start -> Start");
    const PEG overridden(nullable, {{start, tx}});
    CHECK(overridden.accepts({x}));

    const PEG right(grammar, {
        {start, P::optional(P::sequence({tx, P::rule(start), ty}))}, {rule, P::fail()}
    });
    CHECK(right.accepts({x, x, y, y}) && right.accepts({}));
    CHECK(!right.accepts({x, y, y}));
    const PEG consumingPrefix(grammar, {
        {start, P::optional(P::sequence({P::rule(rule), P::rule(start)}))},
        {rule, P::oneOrMore(tx)}
    });
    CHECK(consumingPrefix.accepts({x, x}));
    expectError([&] { (void)PEG(grammar); }, "no PEG expression for rule");
    expectError([&] {
        (void)PEG(grammar, {{rule, P::token(grammar.eof())}});
    }, "invalid PEG token symbol");
    expectError([&] {
        (void)PEG(grammar, {{rule, P::rule(x)}});
    }, "invalid PEG rule reference");
}

void testCombinators() {
    using namespace combinator;
    const Parentheses g;
    const auto tx = token(g.open);
    const auto ty = token(g.close);
    const auto pair = tx.then(ty).map([](auto) { return 1u; });
    const auto choice = pair.orElse(tx.map([](Symbol) { return 2u; }));
    CHECK(choice.parse({g.open, g.close}) == 1);
    CHECK(choice.parse(g.grammar, {g.open}) == 2);
    CHECK(choice.match({g.close, g.open}, 1)->end == 2);
    CHECK(!choice.parse({g.open, g.close, g.open}));
    CHECK(!choice.match({g.close}));
    CHECK(pair.many(1).parse({g.open, g.close, g.open, g.close}) == std::vector<unsigned>({1, 1}));
    CHECK(!pair.many(1).parse({}));
    CHECK(pair.many().parse({}) == std::vector<unsigned>{});
    const auto absent = pair.optional().parse({});
    CHECK(absent && !*absent);
    const auto present = pair.optional().parse({g.open, g.close});
    CHECK(present && *present == 1);
    CHECK(tx.lookahead().then(tx).parse({g.open}));
    CHECK(tx.notFollowedBy().then(ty).then(end()).parse({g.close}));
    CHECK(!tx.notFollowedBy().match({g.open}));
    const auto moved = tx.map([](Symbol) { return std::make_unique<int>(4); }).parse({g.open});
    CHECK(moved && **moved == 4);
    expectError([&] { (void)pure(0).many().parse({}); }, "did not consume");
    expectError<std::out_of_range>([&] { (void)tx.match({}, 1); }, "outside input");
    const ParserCombinator<int> invalid([](const auto&, auto start) {
        return CombinatorMatch<int>{0, start + 2};
    });
    expectError<std::logic_error>([&] { (void)invalid.match({}); }, "invalid end position");
}

void testInvalidInputs() {
    const Parentheses g;
    const LLTableDriven table(g.grammar);
    const LLFunctionDriven functions(g.grammar);
    const GLL gll(g.grammar);
    const ATN atn(g.grammar);
    const PEG peg(g.grammar);
    for (const auto symbol : {g.grammar.eof(), g.start, g.grammar.symbols().size()}) {
        const std::vector<Symbol> tokens{symbol};
        expectError([&] { (void)table.parse(tokens); }, "invalid input");
        expectError([&] { (void)functions.parse(tokens); }, "invalid input");
        expectError([&] { (void)gll.accepts(tokens); }, "invalid input");
        expectError([&] { (void)atn.parse(tokens); }, "invalid input");
        expectError([&] { (void)peg.matchPrefix(tokens); }, "invalid input");
        expectError([&] { (void)RecursiveDescent(g.grammar, tokens); }, "invalid input");
        expectError([&] { (void)combinator::end().parse(g.grammar, tokens); }, "invalid input");
    }
    Grammar invalid;
    expectError([&] { (void)LLTable(invalid); }, "nonterminal");
    expectError([&] { (void)GLL(invalid); }, "nonterminal");
    expectError([&] { (void)ATN(invalid); }, "nonterminal");
    expectError([&] { (void)PEG(invalid); }, "start rule");
}

} // namespace

int main() {
    try {
        testLL();
        testRecursiveDescent();
        testGeneralized();
        testGeneralizedAgainstOracle();
        testPEG();
        testCombinators();
        testInvalidInputs();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
