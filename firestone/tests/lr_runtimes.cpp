#include "Test.hpp"
#include "liblr/GLL.hpp"
#include "liblr/GLR.hpp"
#include "liblr/LLFunctionDriven.hpp"
#include "liblr/PEG.hpp"
#include "liblr/ParserCombinator.hpp"
#include "liblr/RecursiveDescent.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace firestone::lr;

void prefixSemantics() {
    const auto grammar = readEBNF("S = 'x' | 'x' 'y' | ; unused = 'z';");
    const auto x = grammar.findTerminal("x").value();
    const auto y = grammar.findTerminal("y").value();
    const auto z = grammar.findTerminal("z").value();
    const std::vector<Symbol> tokens{z, x, y, z};
    const GLL generalized(grammar);
    const PEG ordered(grammar);
    CHECK(generalized.matchPrefix({x, y, z}) == 2);
    CHECK(ordered.matchPrefix({x, y, z}) == 1);
    CHECK(generalized.matchPrefixAt(tokens, 1) == 3);
    CHECK(ordered.matchPrefixAt(tokens, 1) == 2);
    CHECK(!generalized.accepts(tokens) && !ordered.accepts(tokens));
    const auto longest = generalized.parsePrefix(tokens, 1);
    const auto first = ordered.parsePrefix(tokens, 1);
    CHECK(longest && longest->begin == 1 && longest->end == 3 && longest->children.size() == 2);
    CHECK(longest->children[1].symbol == y && longest->children[1].begin == 2);
    CHECK(first && first->begin == 1 && first->end == 2 && first->children.size() == 1);
    const auto empty = generalized.parsePrefix(tokens, tokens.size());
    CHECK(empty && empty->begin == tokens.size() && empty->end == tokens.size() && empty->children.empty());
    CHECK(ordered.matchPrefixAt(tokens, tokens.size()) == tokens.size());
    throws<std::out_of_range>([&] { generalized.parsePrefix(tokens, tokens.size() + 1); });
    throws<std::out_of_range>([&] { ordered.matchPrefixAt(tokens, tokens.size() + 1); });
    throws<std::invalid_argument>([&] { generalized.matchPrefixAt({grammar.eof(), x}, 1); });
    throws<std::invalid_argument>([&] { ordered.parsePrefix({grammar.start(), x}, 1); });

    const GLL nonempty(readEBNF("S = 'x'; other = 'y';"));
    const auto other = nonempty.grammar().findTerminal("y").value();
    CHECK(!nonempty.matchPrefix({other}) && !nonempty.parsePrefix({other}));
}

void pegMemoAndRollback() {
    using P = PEGExpression;
    Grammar grammar;
    const auto x = grammar.terminal("x"), y = grammar.terminal("y"), z = grammar.terminal("z");
    const auto s = grammar.nonterminal("S"), a = grammar.nonterminal("A");
    grammar.setStart(s);
    const auto ref = P::rule(a);
    const PEG parser(grammar, {
        {s, P::sequence({P::andPredicate(ref),
                         P::choice({P::sequence({ref, P::token(z)}), ref}),
                         P::optional(P::token(y))})},
        {a, P::oneOrMore(P::token(x))}
    });
    const std::vector<Symbol> tokens{z, x, x, y, z};
    const auto tree = parser.parsePrefix(tokens, 1);
    CHECK(tree && tree->begin == 1 && tree->end == 4);
    CHECK(tree->children.size() == 2);
    const auto& aTree = tree->children[0];
    CHECK(aTree.symbol == a && aTree.begin == 1 && aTree.end == 3 && aTree.children.size() == 2);
    CHECK(aTree.children[0].begin == 1 && aTree.children[1].begin == 2);
    CHECK(tree->children[1].symbol == y && tree->children[1].begin == 3);
    CHECK(parser.matchPrefixAt(tokens, 1) == 4);
    CHECK(!parser.parsePrefix(tokens) && !parser.matchPrefixAt(tokens, 4));
}

void predictiveSpansAndDeepRecursion() {
    const auto grammar = readEBNF("S = A '!' B; A = 'x' A | ; B = 'y' | ;");
    const auto x = grammar.findTerminal("x").value(), y = grammar.findTerminal("y").value();
    const auto bang = grammar.findTerminal("!").value();
    const LLFunctionDriven functions(grammar);
    const auto tree = functions.parse({x, x, bang, y});
    CHECK(tree && tree->children.size() == 3);
    CHECK(tree->children[0].begin == 0 && tree->children[0].end == 2);
    CHECK(tree->children[0].children[1].children[1].begin == 2);
    CHECK(tree->children[1].symbol == bang && tree->children[1].begin == 2 && tree->children[1].end == 3);
    CHECK(tree->children[2].begin == 3 && tree->children[2].end == 4);
    CHECK(functions.accepts({bang}) && !functions.accepts({x, y}));
    throws<std::invalid_argument>([&] { functions.accepts({grammar.eof()}); });

    const auto deepGrammar = readEBNF("S = 'x' S 'z' | 'y';");
    const auto dx = deepGrammar.findTerminal("x").value();
    const auto dy = deepGrammar.findTerminal("y").value();
    const auto dz = deepGrammar.findTerminal("z").value();
    const LLFunctionDriven ll(deepGrammar);
    const PEG peg(deepGrammar);
    constexpr std::size_t depth = 50000;
    std::vector<Symbol> nested(depth, dx);
    nested.push_back(dy);
    nested.insert(nested.end(), depth, dz);
    CHECK(ll.accepts(nested) && peg.accepts(nested));
    nested.pop_back();
    CHECK(!ll.parse(nested) && !peg.parse(nested));
}

void glrRecognitionAndWitness() {
    const auto grammar = readEBNF("S = A A 'x'; A = B | ; B = A;");
    const auto a = grammar.findNonterminal("A").value();
    const auto x = grammar.findTerminal("x").value();
    for (const auto mode : {LRMode::canonicalLR1, LRMode::lalr1}) {
        const GLRParser parser(grammar, mode);
        CHECK(parser.accepts({x}) && !parser.accepts({}) && !parser.accepts({x, x}));
        throws<std::invalid_argument>([&] { parser.accepts({grammar.eof()}); });
        const auto forest = parser.parse({x});
        CHECK(forest);
        const auto& children = forest->nodes[forest->root].alternatives[0].children;
        CHECK(children.size() == 3 && children[0] == children[1]);
        const auto& alternatives = forest->nodes[children[0]].alternatives;
        CHECK(alternatives.size() == 2 && !alternatives[0].children.empty()); // cyclic branch sorts first
        const auto tree = forest->tree();
        CHECK(tree && tree->symbol == grammar.start() && tree->begin == 0 && tree->end == 1);
        CHECK(tree->children.size() == 3);
        for (std::size_t i = 0; i < 2; ++i) {
            CHECK(tree->children[i].symbol == a && tree->children[i].children.empty());
            CHECK(tree->children[i].begin == 0 && tree->children[i].end == 0);
        }
        CHECK(tree->children[2].symbol == x && tree->children[2].begin == 0 && tree->children[2].end == 1);
    }
    auto cyclic = ParseForest{0, {{a, 0, 0, false, {{0, {0}}}}}};
    CHECK(!cyclic.tree());
    cyclic.nodes[0].alternatives[0].children[0] = 1;
    throws<std::out_of_range>([&] { cyclic.tree(); });
    throws<std::out_of_range>([] { ParseForest{}.tree(); });
}

void compositionAndCursorTransactions() {
    using namespace combinator;
    const Symbol x = 1, y = 2;
    const auto tx = token(x);
    const auto dependent = satisfy([](Symbol) { return true; })
        .map([](Symbol value) { return std::make_unique<Symbol>(value); })
        .flatMap([](std::unique_ptr<Symbol> value) { return token(*value); });
    CHECK(dependent.parse({x, x}) == x && dependent.parse({y, y}) == y);
    CHECK(!dependent.parse({x, y}) && dependent.orElse(tx).match({x, y})->end == 1);

    const auto repeat = tx.repeat(1, 2);
    const auto match = repeat.match({y, x, x, x}, 1);
    CHECK(match && match->end == 3 && match->value == std::vector<Symbol>({x, x}));
    CHECK(!repeat.parse({x, x, x}) && !repeat.parse({y}));
    CHECK(pure(0).repeat(0, 0).match({y})->end == 0); // body was not invoked
    throws<std::invalid_argument>([&] { tx.repeat(2, 1); });
    throws<std::invalid_argument>([] { pure(0).repeat(0, 1).parse({}); });

    const std::vector<Symbol> tokens{x, y, x};
    RecursiveDescent cursor(tokens);
    CHECK(!cursor.consumeSequence({x, x}) && cursor.remaining() == 3);
    CHECK(cursor.lookahead([&](auto& p) { return p.consumeSequence({x, y}); }));
    CHECK(cursor.position() == 0);
    CHECK(!cursor.lookahead([&](auto& p) { return p.consume(x) && p.consume(x); }));
    CHECK(cursor.position() == 0);
    CHECK(cursor.consumeSequence({x, y}) && cursor.remaining() == 1);
    throws<std::runtime_error>([&] {
        cursor.lookahead([](auto& p) -> bool { p.take(); throw std::runtime_error("probe"); });
    });
    CHECK(cursor.position() == 2 && cursor.consumeSequence({x}) && cursor.atEnd());
    CHECK(cursor.consumeSequence({}) && !cursor.consumeSequence({y}));
}

} // namespace

int main() { return test_main([] {
    prefixSemantics();
    pegMemoAndRollback();
    predictiveSpansAndDeepRecursion();
    glrRecognitionAndWitness();
    compositionAndCursorTransactions();
}); }
