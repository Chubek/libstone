#include "Test.hpp"
#include "liblr/GLR.hpp"
#include "liblr/LALR.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

using namespace firestone::lr;

namespace {

void checkForest(const Grammar& grammar, const std::vector<Symbol>& input, const ParseForest& forest) {
    const auto& root = forest.nodes.at(forest.root);
    CHECK(root.symbol == grammar.start() && root.begin == 0 && root.end == input.size());
    for (const auto& node : forest.nodes) {
        CHECK(node.begin <= node.end && node.end <= input.size());
        CHECK(node.terminal == grammar.symbols().at(node.symbol).terminal);
        if (node.terminal) {
            CHECK(node.end == node.begin + 1 && node.symbol == input.at(node.begin));
            CHECK(node.alternatives.empty());
        } else {
            CHECK(!node.alternatives.empty());
            for (const auto& choice : node.alternatives) {
                const auto& rule = grammar.productions().at(choice.production);
                CHECK(rule.lhs == node.symbol && rule.rhs.size() == choice.children.size());
                auto position = node.begin;
                for (std::size_t i = 0; i < choice.children.size(); ++i) {
                    const auto& child = forest.nodes.at(choice.children[i]);
                    CHECK(child.symbol == rule.rhs[i] && child.begin == position);
                    position = child.end;
                }
                CHECK(position == node.end);
            }
        }
    }
}

std::size_t treeCount(const ParseForest& forest) {
    std::vector<bool> visiting(forest.nodes.size());
    std::vector<std::size_t> memo(forest.nodes.size());
    const std::function<std::size_t(ForestNodeId)> count = [&](ForestNodeId id) -> std::size_t {
        CHECK(!visiting.at(id)); // Only for the acyclic test cases.
        if (memo[id]) return memo[id];
        const auto& node = forest.nodes[id];
        if (node.terminal) return 1;
        visiting[id] = true;
        for (const auto& alternative : node.alternatives) {
            std::size_t ways = 1;
            for (auto child : alternative.children) ways *= count(child);
            memo[id] += ways;
        }
        visiting[id] = false;
        return memo[id];
    };
    return count(forest.root);
}

void grammarAndEBNF() {
    Grammar grammar;
    CHECK(grammar.symbols().at(grammar.eof()).terminal);
    throws<std::invalid_argument>([&] { grammar.validate(); });
    const auto word = grammar.terminal("word"), start = grammar.nonterminal("word");
    CHECK(grammar.terminal("word") == word && grammar.nonterminal("word") == start && word != start);
    throws<std::invalid_argument>([&] { grammar.terminal(grammar.symbols()[grammar.eof()].name); });
    throws<std::invalid_argument>([&] { grammar.nonterminal(""); });
    throws<std::invalid_argument>([&] { grammar.setStart(word); });
    throws<std::invalid_argument>([&] { grammar.setStart(93); });
    grammar.setStart(start);
    throws<std::invalid_argument>([&] { grammar.addProduction(word, {}); });
    throws<std::invalid_argument>([&] { grammar.addProduction(start, {grammar.eof()}); });
    throws<std::invalid_argument>([&] { grammar.addProduction(start, {98}); });
    const auto undefined = grammar.nonterminal("undefined");
    CHECK(grammar.addProduction(start, {word, undefined}) == 0);
    throws<std::invalid_argument>([&] { grammar.validate(); });
    throws<std::invalid_argument>([&] { generateLRTable(grammar); });
    CHECK(grammar.addProduction(undefined, {}) == 1);
    grammar.validate();
    validateInput(grammar, {word});
    throws<std::invalid_argument>([&] { validateInput(grammar, {start}); });
    throws<std::invalid_argument>([&] { validateInput(grammar, {grammar.eof()}); });
    throws<std::invalid_argument>([&] { validateInput(grammar, {100}); });

    const auto ebnf = readEBNF(R"(
        # The declaration may name a later production.
        head = 'h' | "j";
        %start sequence;
        sequence ::= head (',' head)* ['!'] {'?'} 'z'+ '\t'?;
    )");
    CHECK(ebnf.start() == ebnf.findNonterminal("sequence").value());
    auto token = [&](const std::string& name) { return ebnf.findTerminal(name).value(); };
    const LRParser parse(ebnf);
    const auto tree = parse.parse({token("h"), token(","), token("j"), token("!"), token("?"), token("?"),
                                   token("z"), token("z"), token("\t")});
    CHECK(tree && tree->symbol == ebnf.start() && tree->begin == 0 && tree->end == 9);
    CHECK(parse.parse({token("j"), token("z")}));
    CHECK(!parse.parse({token("j")}));
    CHECK(!parse.parse({token("j"), token(","), token("z")}));
    const auto epsilon = readEBNF("first = ; second = 'x';");
    CHECK(epsilon.start() == epsilon.findNonterminal("first").value());
    const auto empty = LRParser(epsilon).parse({});
    CHECK(empty && empty->symbol == epsilon.start() && empty->begin == 0 && empty->end == 0 &&
          empty->children.empty());

    // A simple postfix + stays predictive: only the tail is optional.
    const auto oneOrMore = readEBNF("S = 'word'+;");
    const auto item = oneOrMore.findTerminal("word").value();
    const auto& rules = oneOrMore.productions();
    CHECK(rules.size() == 4 && rules.back().lhs == oneOrMore.start() && rules.back().rhs.size() == 1);
    const auto repeat = rules.back().rhs.front();
    const Production* repeatRule = nullptr;
    for (const auto& rule : rules) {
        if (rule.lhs != repeat) continue;
        CHECK(!repeatRule);
        repeatRule = &rule;
    }
    CHECK(repeatRule && repeatRule->rhs.size() == 2 && repeatRule->rhs.front() == item);
    const auto tail = repeatRule->rhs.back();
    std::vector<std::vector<Symbol>> tailRules;
    for (const auto& rule : rules) {
        if (rule.lhs == tail) tailRules.push_back(rule.rhs);
    }
    CHECK(tailRules == std::vector<std::vector<Symbol>>{{item, tail}, {}});
    const LRParser repeatParser(oneOrMore);
    CHECK(!repeatParser.parse({}) && repeatParser.parse({item}) && repeatParser.parse({item, item}));

    for (const auto* invalid : {"", "s = ('x';", "s = missing;", "s = 'x'", "s = '\\q';",
                                "s = '';", "s = '$end';", "s = 'x'**;", "%start t; s = 'x';"}) {
        throws<std::invalid_argument>([&] { readEBNF(invalid); });
    }
    throws<std::invalid_argument>([&] { readEBNF(std::string("S = 'x';\0junk", 13)); });
    throws<std::invalid_argument>([&] { readEBNF("S = " + std::string(500, '(')); });
}

void lrTablesAndTrees() {
    const auto grammar = readEBNF("expr = expr '+' term | term; term = term '*' atom | atom; atom = 'id';");
    const auto expr = grammar.start(), term = grammar.findNonterminal("term").value();
    const auto id = grammar.findTerminal("id").value(), plus = grammar.findTerminal("+").value();
    const auto times = grammar.findTerminal("*").value();
    const auto canonical = generateLRTable(grammar);
    const auto lalr = generateLRTable(grammar, LRMode::lalr1);
    CHECK(canonical.mode() == LRMode::canonicalLR1 && lalr.mode() == LRMode::lalr1);
    CHECK(canonical.conflicts().empty() && lalr.conflicts().empty());
    CHECK(lalr.stateCount() <= canonical.stateCount());
    CHECK(lalr.goTo(0, expr));
    CHECK(lalr.actions(0, id).size() == 1 && lalr.actions(0, id)[0].kind == LRActionKind::shift);
    const LRParser parser(lalr);
    const auto tree = parser.parse({id, plus, id, times, id});
    CHECK(tree && tree->symbol == expr && tree->begin == 0 && tree->end == 5);
    CHECK(tree->children.size() == 3 && tree->children[0].end == 1 && tree->children[1].symbol == plus);
    CHECK(tree->children[1].begin == 1 && tree->children[1].end == 2 && tree->children[1].children.empty());
    const auto& product = tree->children[2];
    CHECK(product.symbol == term && product.begin == 2 && product.end == 5);
    CHECK(product.children.size() == 3 && product.children[1].symbol == times);
    CHECK(!parser.parse({id, plus, times, id}) && !parser.parse({id, id}) && !parser.parse({}));
    throws<std::invalid_argument>([&] { parser.parse({id, grammar.eof()}); });

    const auto nullable = readEBNF("S = A B C; A = ; B = | 'b'; C = | 'c';");
    const LRParser withEpsilon(nullable);
    const auto c = nullable.findTerminal("c").value();
    const auto onlyC = withEpsilon.parse({c});
    CHECK(onlyC && onlyC->children.size() == 3);
    CHECK(onlyC->children[0].begin == 0 && onlyC->children[0].end == 0);
    CHECK(onlyC->children[1].begin == 0 && onlyC->children[1].end == 0);
    CHECK(onlyC->children[2].begin == 0 && onlyC->children[2].end == 1);
    CHECK(withEpsilon.parse({}));

    // This is LALR(1), but FOLLOW-based SLR reduction would conflict.
    const auto notSLR = readEBNF("S = L '=' R | R; L = '*' R | 'id'; R = L;");
    CHECK(generateLRTable(notSLR, LRMode::lalr1).conflicts().empty());
    const auto leaf = notSLR.findTerminal("id").value(), eq = notSLR.findTerminal("=").value();
    CHECK(LRParser(notSLR).parse({leaf, eq, leaf}));
}

void verifyCoreMerge() {
    const auto grammar = readEBNF("S = 'a' A 'd' | 'b' A 'e' | 'a' B 'e' | 'b' B 'd'; A = 'c'; B = 'c';");
    const auto canonical = generateLRTable(grammar, LRMode::canonicalLR1);
    const auto lalr = generateLRTable(grammar, LRMode::lalr1);
    CHECK(canonical.conflicts().empty() && canonical.stateCount() == lalr.stateCount() + 1);
    CHECK(lalr.conflicts().size() == 2);
    auto token = [&](const std::string& name) { return grammar.findTerminal(name).value(); };
    for (const auto& conflict : lalr.conflicts()) {
        CHECK(conflict.lookahead == token("d") || conflict.lookahead == token("e"));
        CHECK(conflict.actions == lalr.actions(conflict.state, conflict.lookahead) && conflict.actions.size() == 2);
        CHECK(conflict.actions[0].kind == LRActionKind::reduce && conflict.actions[1].kind == LRActionKind::reduce);
        CHECK(conflict.actions[0].target != conflict.actions[1].target);
    }
    throws<std::invalid_argument>([&] { LRParser parser(lalr); });
    const LRParser deterministic(canonical);
    const GLRParser general(lalr);
    for (const auto prefix : {token("a"), token("b")}) {
        for (const auto suffix : {token("d"), token("e")}) {
            const std::vector<Symbol> input{prefix, token("c"), suffix};
            CHECK(deterministic.parse(input));
            const auto forest = general.parse(input);
            CHECK(forest);
            checkForest(grammar, input, *forest);
            CHECK(treeCount(*forest) == 1);
        }
    }
    CHECK(!general.parse({token("a"), token("c"), token("c")}));
}

void ambiguityAndCycles() {
    const auto ambiguous = readEBNF("S = S S | 'x';");
    const GLRParser parser(ambiguous);
    const auto x = ambiguous.findTerminal("x").value();
    CHECK(!parser.table().conflicts().empty());
    const auto& conflict = parser.table().conflicts().front();
    CHECK(conflict.lookahead == x && conflict.actions.size() == 2);
    CHECK(conflict.actions[0].kind == LRActionKind::shift && conflict.actions[1].kind == LRActionKind::reduce);
    const std::vector<Symbol> input(5, x);
    const auto forest = parser.parse(input);
    CHECK(forest && forest->nodes.at(forest->root).alternatives.size() == 4);
    checkForest(ambiguous, input, *forest);
    CHECK(treeCount(*forest) == 14);
    CHECK(!parser.parse({}));
    throws<std::invalid_argument>([&] { parser.parse({ambiguous.eof()}); });

    const auto epsilonChoice = readEBNF("S = A A; A = | 'x';");
    const std::vector<Symbol> one{epsilonChoice.findTerminal("x").value()};
    const auto both = GLRParser(epsilonChoice).parse(one);
    CHECK(both);
    checkForest(epsilonChoice, one, *both);
    CHECK(treeCount(*both) == 2);

    const auto nullable = readEBNF("S = S S | ;");
    const auto empty = GLRParser(nullable).parse({});
    CHECK(empty);
    checkForest(nullable, {}, *empty);
    const auto& root = empty->nodes.at(empty->root);
    CHECK(empty->nodes.size() == 1 && root.alternatives.size() == 2);
    CHECK(root.alternatives[0].children == std::vector<ForestNodeId>({empty->root, empty->root}));
    CHECK(root.alternatives[1].children.empty());

    const auto cycle = readEBNF("S = A; A = B | 'x'; B = A;");
    const std::vector<Symbol> cycleInput{cycle.findTerminal("x").value()};
    const auto cyclic = GLRParser(cycle).parse(cycleInput);
    CHECK(cyclic);
    checkForest(cycle, cycleInput, *cyclic);
    const auto a = cyclic->nodes.at(cyclic->root).alternatives.at(0).children.at(0);
    const auto b = cyclic->nodes.at(a).alternatives.at(0).children.at(0);
    CHECK(cyclic->nodes.at(b).alternatives.at(0).children == std::vector<ForestNodeId>{a});

    const auto unproductive = readEBNF("S = A; A = S; dead = 'x';");
    const GLRParser never(unproductive);
    CHECK(!never.parse({}) && !never.parse({unproductive.findTerminal("x").value()}));

    const auto duplicate = readEBNF("S = | ;");
    const auto identical = GLRParser(duplicate).parse({});
    CHECK(identical && treeCount(*identical) == 2);
}

} // namespace

int main() { return test_main([] {
    grammarAndEBNF();
    lrTablesAndTrees();
    verifyCoreMerge();
    ambiguityAndCycles();
}); }
