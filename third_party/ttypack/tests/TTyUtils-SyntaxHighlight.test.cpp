#include <TTyUtils/TTyUtils-SyntaxHighlight.hpp>
#include <cassert>
#include <utility>
#include <vector>

int main() {
    ttyutils::syntax::Grammar grammar;
    grammar.keywords = {"", "int"};
    grammar.root_scope = ttyutils::syntax::Scope{"keyword"};
    grammar.rules = {{"", R"(/\*)", R"(\*/)", ttyutils::syntax::Scope{"comment"}}};
    ttyutils::textbuf::Buffer buffer{"int /*\nint\n*/ int\nint"};
    ttyutils::theme::Theme theme;
    ttyutils::theme::Style style{.attributes = 1};
    theme.set_role("keyword", style);
    ttyutils::hl::Highlighter highlighter{grammar, theme};
    highlighter.attach(buffer);
    assert(highlighter.style(grammar.root_scope) == style);
    std::vector<std::pair<int, int>> ready;
    highlighter.on_ready([&](int first, int last) { ready.emplace_back(first, last); });
    auto last = highlighter.line_spans(3);
    assert(last.items().size() == 1);
    assert(highlighter.line_spans(0).items().size() == 2);
    assert(highlighter.line_spans(1).items()[0].scope.name() == "comment");
    assert(highlighter.line_spans(2).items().size() == 2);
    assert((ready == std::vector<std::pair<int,int>>{{0, 3}}));
    buffer.erase({0, 4}, {0, 6});
    assert(highlighter.line_spans(3).items().size() == 1);
    assert(highlighter.line_spans(1).items()[0].scope.name() == "keyword");
    assert((ready.back() == std::pair{0, 2}));
    highlighter.set_line_limit(2);
    assert(highlighter.line_spans(2).items().size() == 1);
    ttyutils::textbuf::Buffer another{"plain"};
    highlighter.attach(another);
    assert(highlighter.line_spans(0).items().empty());
    return 0;
}
