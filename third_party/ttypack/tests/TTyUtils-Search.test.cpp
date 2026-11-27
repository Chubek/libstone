#include <TTyUtils/TTyUtils-Search.hpp>
#include <cassert>
#include <vector>

int main() {
    ttyutils::textbuf::Buffer buffer{"TODO todo"};
    auto query = ttyutils::search::Query::literal("todo", false);
    assert(ttyutils::search::find_all(buffer.snapshot(), query).size() == 2);
    ttyutils::search::Replacer replacer{buffer, query, "FIX"};
    assert(replacer.replace_all() == 2);
    assert(buffer.text() == "FIX FIX");
    using namespace ttyutils::search;
    using ttyutils::textbuf::Buffer;
    const Buffer unicode{"CAFÉ Καλημέρα cat. catX"};
    assert(find(unicode.snapshot(), Query::literal("café", false))->end.col == 4);
    assert(find(unicode.snapshot(), Query::literal("καλημέρα", false))->begin.col == 5);
    assert(find_all(unicode.snapshot(), Query::regex(R"(\bcat\.)")).size() == 1);
    assert(!find(unicode.snapshot(), Query::regex("(unclosed")));
    assert(!find(unicode.snapshot(), Query::regex("^(a|b)+$")));
    assert(find(Buffer{}.snapshot(), Query::regex("^$")));
    const auto end = find(unicode.snapshot(), Query::regex("$"));
    assert(end && end->begin == end->end && end->begin.col == unicode.line_index().columns(0));
    const auto greedy = find(Buffer{"aaa"}.snapshot(), Query::regex("a+"));
    const auto lazy = find(Buffer{"aaa"}.snapshot(), Query::regex("a+?"));
    assert(greedy->end.col == 3 && lazy->end.col == 1);
    assert(!find(Buffer{"e\xCC\x81"}.snapshot(), Query::literal("e")));
    assert(find(Buffer{"e\xCC\x81"}.snapshot(), Query::regex("..")));
    assert(!find(Buffer{"\r\n"}.snapshot(), Query::literal("\r")));
    assert(find(Buffer{"\r\n"}.snapshot(), Query::regex(R"(\r\n)")));
    Buffer captures{"key:12 id:9 abc:10"};
    Replacer replace{captures, Query::regex(R"(\b([a-z]+):(\d+)\b)"), R"($2=\1 $$)"};
    std::vector<std::size_t> seen;
    assert(replace.confirm_each([&](const Match& m) {
        seen.push_back(m.range.begin.col);
        return m.captures[1] != "id";
    }) == 2);
    assert((seen == std::vector<std::size_t>{0, 7, 12}));
    assert(captures.text() == "12=key $ id:9 10=abc $");
    const auto fuzzy = find(Buffer{"xÉcho Zulu"}.snapshot(), Query::fuzzy("éz"));
    assert(fuzzy && fuzzy->begin.col == 1);
    IncrementalState state;
    Buffer typed{"aaab"};
    assert(incremental(typed.snapshot(), Query::literal("aa"), state).front().begin.col == 0);
    assert(incremental(typed.snapshot(), Query::literal("aab"), state).front().begin.col == 1);
    typed.insert({0, 0}, "-");
    assert(incremental(typed.snapshot(), Query::literal("aab"), state).front().begin.col == 2);
    assert(!find(Buffer{std::string(5000, 'a')}.snapshot(), Query::regex("(a|aa)*b")));
    return 0;
}
