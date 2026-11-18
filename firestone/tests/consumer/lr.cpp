#include <liblr/LR.hpp>

#include <cstddef>
#include <vector>

int main() {
    using namespace firestone::lr;
    const auto grammar = readEBNF("sum = 'word' { '+' 'word' };");
    const auto word = grammar.findTerminal("word");
    const auto plus = grammar.findTerminal("+");
    if (!word || !plus) return 1;
    if (!validate_utf8("alpha + beta") || validate_utf8("\xed\xa0\x80")) return 1;
    const auto lexer = make_lexer({{"[a-z]+", *word}, {R"(\+)", *plus}, {"[ \t]+", 0, true}});
    std::vector<Symbol> tokens;
    std::size_t cursor = 0;
    for (;;) {
        const auto token = lexer.next("alpha + beta", cursor);
        if (token.status == LexStatus::End) break;
        if (token.status != LexStatus::Token) return 1;
        tokens.push_back(token.token);
    }
    if (tokens != std::vector<Symbol>{*word, *plus, *word}) return 1;

    const auto lr = LRParser(grammar).parse(tokens);
    const auto ll = LLTableDriven(grammar).parse(tokens);
    return lr && ll && PEG(grammar).accepts(tokens) &&
        lr->symbol == grammar.start() && ll->symbol == grammar.start() &&
        lr->begin == 0 && ll->begin == 0 && lr->end == tokens.size() && ll->end == tokens.size() ? 0 : 1;
}
