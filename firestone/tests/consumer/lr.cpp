#include <liblr/LR.hpp>

#include <variant>
#include <vector>

int main() {
    using namespace firestone::lr;
    const auto grammar = readEBNF("sum = 'word' { '+' 'word' };");
    const auto word = grammar.findTerminal("word");
    const auto plus = grammar.findTerminal("+");
    if (!word || !plus) return 1;
    if (!validate_utf8("alpha + beta") || validate_utf8("\xed\xa0\x80")) return 1;
    const auto lexer = make_lexer({{"[a-z]+", *word}, LexerRule::literal("+", *plus), {"[ \t]+", 0, true}});
    const auto lexed = lexSymbols(lexer, grammar, "alpha + beta");
    const auto* tokens = std::get_if<std::vector<Symbol>>(&lexed);
    if (!tokens || *tokens != std::vector<Symbol>{*word, *plus, *word}) return 1;

    const auto lr = LRParser(grammar, LRMode::slr1).parse(*tokens);
    const auto ll = LLTableDriven(grammar).parse(*tokens);
    return lr && ll && PEG(grammar).matchPrefixAt(*tokens, 0) == tokens->size() &&
        lr->symbol == grammar.start() && ll->symbol == grammar.start() &&
        lr->begin == 0 && ll->begin == 0 && lr->end == tokens->size() && ll->end == tokens->size() ? 0 : 1;
}
