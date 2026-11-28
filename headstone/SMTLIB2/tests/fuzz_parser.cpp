#include <smtlib2/ast.hpp>
#include <cstdint>
#include <cstdlib>

// Optional libFuzzer entry point; no fuzzing runtime is needed by the library
// or regular test suite.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace smtlib2;
    constexpr Limits limits{4096, 4096, 128};
    if (size > limits.max_input_bytes) return 0;
    const std::string_view input(reinterpret_cast<const char*>(data), size);

    std::vector<Token> tokens;
    try {
        tokens = lex(input, limits);
    } catch (const Error&) {
        return 0;
    }
    std::size_t previous_end = 0;
    for (const auto& token : tokens) {
        const auto begin = token.span.begin.offset, end = token.span.end.offset;
        if (begin < previous_end || end <= begin || end > size || input.substr(begin, end - begin) != token.text)
            std::abort();
        previous_end = end;
    }

    std::vector<SExpr> roots;
    try {
        roots = parse(input, limits);
    } catch (const Error&) {
        return 0;
    }
    std::string canonical;
    for (const auto& root : roots) {
        if (!canonical.empty()) canonical += ' ';
        canonical += print(root);
    }
    // Reformatting may add bytes to punctuation-adjacent inputs.
    const Limits canonical_limits{canonical.size(), limits.max_tokens, limits.max_depth};
    const auto again = lex(canonical, canonical_limits);
    if (again.size() != tokens.size()) std::abort();
    for (std::size_t i = 0; i < again.size(); ++i)
        if (again[i].kind != tokens[i].kind || again[i].text != tokens[i].text) std::abort();

    ScriptSyntax script;
    try {
        script = make_script(std::move(roots));
    } catch (const Error&) {
        return 0;
    }
    const auto rendered = print(script);
    const auto reparsed = parse_script(rendered, canonical_limits);
    if (print(reparsed) != rendered || reparsed.commands.size() != script.commands.size()) std::abort();
    for (std::size_t i = 0; i < reparsed.commands.size(); ++i)
        if (reparsed.commands[i].kind != script.commands[i].kind) std::abort();
    return 0;
}
