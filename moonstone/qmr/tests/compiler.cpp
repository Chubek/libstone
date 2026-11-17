#include "qmr/Qmr.hpp"
#include <algorithm>
#include <cassert>
#include <string>

using namespace moonstone::qmr;

namespace {
std::size_t lua_line_of(const Program &p, std::string_view needle) {
    const auto at = p.lua_source.find(needle);
    assert(at != std::string::npos);
    return static_cast<std::size_t>(
        std::count(p.lua_source.begin(), p.lua_source.begin() + at, '\n')) + 1;
}
} // namespace

int main() {
    const auto basic = compile("Hello {{ data.user }}!", "fixture.qmr");
    assert(basic.lua_source.find("__qmr_emit((data.user), true)") != std::string::npos);
    assert(basic.lua_source.find("__qmr_write(\"Hello \")") != std::string::npos);
    auto p = compile("{% for i=1,3 do %}{{{ i }}}{% end %}");
    assert(p.lua_source.find("for i=1,3 do") != std::string::npos);
    assert(p.lua_source.find("__qmr_emit((i), false)") != std::string::npos);
    p = compile("A {# ignored #}B {% include 'other.qmr' %}");
    assert(p.lua_source.find("__qmr_include('other.qmr')") != std::string::npos);
    assert(p.lua_source.find("ignored") == std::string::npos);
    p = compile("{{ qmr.default(nil, '}}') }}");
    assert(p.lua_source.find("qmr.default(nil, '}}')") != std::string::npos);
    p = compile("a   {{- 'X' -}}    b");
    assert(p.lua_source.find("__qmr_write(\"a\")") != std::string::npos);
    assert(p.lua_source.find("__qmr_write(\"b\")") != std::string::npos);
    p = compile("\0\1\n", "binary.qmr"); // Literal stops at first NUL (C string test).
    assert(p.lua_source.find("__qmr_write") != std::string::npos);
    assert(escape_html("&<>\"'") == "&amp;&lt;&gt;&quot;&#39;");
    assert(escape_url("a b/") == "a%20b%2F");
    bool rejected = false;
    try { (void)compile("hello\n{{ no", "bad.qmr"); }
    catch (const Error &e) {
        rejected = e.code() == "QMR_UNCLOSED" && e.file() == "bad.qmr" && e.line() == 2;
    }
    assert(rejected);
    Limits tiny;
    tiny.max_template_bytes = 1;
    try { (void)compile("abcd", "small", tiny); rejected = false; }
    catch (const Error &e) { rejected = e.code() == "QMR_SIZE"; }
    assert(rejected);

    // --- Codec escapers -----------------------------------------------------
    assert(escape_xml("&<'\">") == "&amp;&lt;&apos;&quot;&gt;");
    assert(escape_xml("plain") == "plain");
    assert(escape_json("x\"y\\z\nd") == "\"x\\\"y\\\\z\\nd\"");
    assert(escape_json("") == "\"\"");
    std::string json = escape_json(std::string("\x01\x1f", 2));
    assert(json == "\"\\u0001\\u001F\"");

    // --- Lua comments, long brackets and quoted delimiters inside tags ------
    p = compile("{% local s = [[a%}b]] %}");
    assert(p.lua_source.find("local s = [[a%}b]]") != std::string::npos);
    p = compile("{% -- hidden %}\nlocal ok = 1 %}");
    assert(p.lua_source.find("-- hidden %}") != std::string::npos);
    assert(p.lua_source.find("local ok = 1") != std::string::npos);
    p = compile("{% --[[ %} inside ]] local x = 1 %}");
    assert(p.lua_source.find("local x = 1") != std::string::npos);
    p = compile("{% --[==[ %} ]] ]==] local y = 2 %}");
    assert(p.lua_source.find("local y = 2") != std::string::npos);
    p = compile("{{ f('}}') }}");
    assert(p.lua_source.find("f('}}')") != std::string::npos);
    p = compile("{{ f(\"a\\\"}}b\") }}");
    assert(p.lua_source.find("f(\"a\\\"}}b\")") != std::string::npos);
    rejected = false;
    try { (void)compile("{{ f([[unterminated }}"); }
    catch (const Error &e) { rejected = e.code() == "QMR_UNCLOSED"; }
    assert(rejected);
    rejected = false;
    try { (void)compile("{% --[[ unterminated %}"); }
    catch (const Error &e) { rejected = e.code() == "QMR_UNCLOSED"; }
    assert(rejected);
    // A '[...' that is not a long bracket must not swallow the delimiter.
    p = compile("{{ data.list[1] }}");
    assert(p.lua_source.find("data.list[1]") != std::string::npos);

    // --- Source maps: generated Lua line -> template line/column ------------
    const std::string text = "Hello {{ data.user }}!\n"
                             "{% for i=1,3 do %}\n"
                             "x\n"
                             "{% end %}\n";
    const auto mapped = compile(text, "map.qmr");
    assert(mapped.lua_header_lines >= 3);
    std::size_t tline = 0, tcol = 0;
    const auto expr_line = lua_line_of(mapped, "__qmr_emit((data.user), true)");
    assert(mapped.locate_lua_line(expr_line, tline, tcol));
    assert(tline == 1 && tcol == 7);
    const auto loop_line = lua_line_of(mapped, "for i=1,3 do");
    assert(mapped.locate_lua_line(loop_line, tline, tcol));
    assert(tline == 2 && tcol == 1);
    const auto literal_line = lua_line_of(mapped, "__qmr_write(\"\\nx\\n\")");
    assert(mapped.locate_lua_line(literal_line, tline, tcol));
    assert(tline == 2); // origin: first character of the construct, after the tag
    assert(!mapped.locate_lua_line(0, tline, tcol));
    assert(!mapped.locate_lua_line(mapped.lua_header_lines, tline, tcol));
    // A multiline Lua construct maps all its generated lines to the tag.
    const auto multi = compile("A\n{% local a = 1\n   local b = 2 %}\nB", "multi.qmr");
    const auto a_line = lua_line_of(multi, "local a = 1");
    const auto b_line = lua_line_of(multi, "local b = 2");
    assert(a_line + 1 == b_line);
    assert(multi.locate_lua_line(a_line, tline, tcol) && tline == 2 && tcol == 1);
    assert(multi.locate_lua_line(b_line, tline, tcol) && tline == 2 && tcol == 1);

    // --- Limits on generated code and node count -----------------------------
    Limits node_limits;
    node_limits.max_nodes = 3;
    rejected = false;
    try { (void)compile("a{{ 1 }}b{{ 2 }}c{{ 3 }}", "nodes.qmr", node_limits); }
    catch (const Error &e) { rejected = e.code() == "QMR_NODES"; }
    assert(rejected);
    return 0;
}
