#include "qmr/Qmr.hpp"
#include <cassert>
#include <string>

using namespace moonstone::qmr;
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
}
