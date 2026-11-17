// Deterministic property-based tests for the QMR lexer/compiler.
// Lua-free: links only moonstone_qmr_compiler.
#include "qmr/Qmr.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

using namespace moonstone::qmr;

namespace {

// Deterministic xorshift64 PRNG; fixed seeds keep CI reproducible.
struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed ? seed : 0x9e3779b97f4a7c15ull) {}
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % bound); }
};

const std::vector<std::string_view> kAlphabet = {
    "{", "}", "%", "#", "[", "]", "=", "-", "'", "\"", "\\", "\n", " ", "a", "b", "1",
    "{{", "}}", "{{{", "}}}", "{%", "%}", "{#", "#}", "--", "--[[", "]]", "[[", "[==[",
    "==]", "'", "\"x\"", "&", "<", "\t", "\r",
};

std::string random_template(Random &random, std::size_t pieces) {
    std::string out;
    for (std::size_t i = 0; i < pieces; ++i)
        out.append(kAlphabet[random.below(kAlphabet.size())]);
    return out;
}

int failures = 0;
void check(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "qmr lexer fuzz: FAILED property: %s\n", what);
        ++failures;
    }
}

// Property 1: compile() always either succeeds or throws the typed Error —
// no other exception escapes, no crash, no hang.
// Property 2: compilation is deterministic (byte-identical output, maps included).
// Property 3: generated code never exceeds max_compiled_bytes.
// Property 4: every generated Lua line within a construct maps to a template
// position that actually exists in the source.
void check_template(const std::string &source, const Limits &limits) {
    Program first;
    bool ok = false;
    try {
        first = compile(source, "fuzz.qmr", limits);
        ok = true;
    } catch (const Error &) {
        return; // typed rejection is a valid outcome
    }
    check(ok, "unexpected exception type");
    const Program second = compile(source, "fuzz.qmr", limits);
    check(first.lua_source == second.lua_source, "deterministic lua_source");
    check(first.line_map == second.line_map, "deterministic line_map");
    check(first.lua_source.size() <= limits.max_compiled_bytes, "compiled size bound");
    std::size_t total_lua_lines = 1;
    for (char c : first.lua_source)
        if (c == '\n') ++total_lua_lines;
    std::size_t line = 0, column = 0;
    for (std::size_t lua_line = 1; lua_line <= total_lua_lines; ++lua_line) {
        if (first.locate_lua_line(lua_line, line, column)) {
            check(line >= 1 && column >= 1, "mapped origin is 1-based");
            std::size_t template_lines = 1;
            for (char c : source)
                if (c == '\n') ++template_lines;
            check(line <= template_lines, "mapped line within source");
        }
    }
}

} // namespace

int main() {
    Limits defaults;
    // Structured templates around valid Lua constructs.
    const std::vector<std::string> valid_corpus = {
        "Hello {{ data.name }}!",
        "{% local s = [[a%}b]] %}{{ s }}",
        "{% --[[ c %} ]] local x = 1 %}{{ x }}",
        "{{ qmr.escape_url('a b/c') }}",
        "{% for i=1,10 do %}{{ i }}{% end %}",
        "{% if data.a then %}yes{% else %}no{% end %}",
        "{# comment {# nested #} done #}",
        "{{ f('}}') }}",
        "{{ f(\"a\\\"}}b\") }}",
        "{% local t = {['k%}']=1} %}{{ t['k%}'] }}",
        "{{- data.trimmed -}}",
        "{% local s = [==[x]=]y]==] %}{{{ s }}}",
    };

    // Random structured mutations of valid templates.
    Random mutation_random(0x51d3e11a);
    for (int i = 0; i < 20000; ++i) {
        std::string source = valid_corpus[mutation_random.below(valid_corpus.size())];
        const auto mutations = 1 + mutation_random.below(4);
        for (std::size_t m = 0; m < mutations; ++m) {
            if (source.empty()) break;
            const std::size_t at = mutation_random.below(source.size());
            const auto action = mutation_random.below(3);
            if (action == 0)
                source[at] = kAlphabet[mutation_random.below(kAlphabet.size())][0];
            else if (action == 1)
                source.erase(at, 1);
            else
                source.insert(at, std::string(kAlphabet[mutation_random.below(kAlphabet.size())]));
        }
        check_template(source, defaults);
        if (failures) return 1;
    }

    // Fully random strings from a delimiter-rich alphabet.
    Random corpus_random(0xfeedface);
    for (int i = 0; i < 30000; ++i) {
        const auto pieces = corpus_random.below(24);
        check_template(random_template(corpus_random, pieces), defaults);
        if (failures) return 1;
    }

    // Byte-level random strings (including control bytes).
    Random raw_random(0xc0ffee123456789ull);
    for (int i = 0; i < 20000; ++i) {
        std::string source;
        const auto length = raw_random.below(64);
        for (std::size_t j = 0; j < length; ++j)
            source.push_back(static_cast<char>(raw_random.below(256)));
        check_template(source, defaults);
        if (failures) return 1;
    }

    // Delimiter round-trip: text without '{' renders as itself (checked here
    // only for compilation: the whole source must become one literal write).
    Random literal_random(0x1badb002);
    for (int i = 0; i < 500; ++i) {
        std::string source;
        const auto length = literal_random.below(200);
        for (std::size_t j = 0; j < length; ++j) {
            char c;
            do {
                c = static_cast<char>(literal_random.below(128));
            } while (c == '{');
            source.push_back(c);
        }
        if (source.empty()) continue;
        const Program program = compile(source, "literal.qmr");
        const auto needle = "__qmr_write(\"";
        const auto first = program.lua_source.find(needle);
        check(first != std::string::npos, "literal produced a write");
        check(program.lua_source.find("{{", first) == std::string::npos,
              "no interpolation of plain text");
    }

    std::fprintf(stderr, "qmr lexer fuzz: all properties held\n");
    return failures == 0 ? 0 : 1;
}
