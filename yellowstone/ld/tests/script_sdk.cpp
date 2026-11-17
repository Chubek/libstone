#include "ScriptSDK.hpp"
#include "ld.hpp"
#include <iostream>
#include <stdexcept>
#include <memory>

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
int main() try {
    qld::ScriptSDK sdk;
    sdk.defineMacro("symbol", {"name", "value"}, "${name} = ${value};\n");
    sdk.registerDirective("answer", [](const qld::ScriptSDK::Arguments &args) {
        if (!args.empty()) throw std::runtime_error("answer expects no arguments");
        return "@symbol answer, 42\n";
    });
    auto text = sdk.preprocess("@answer\nSECTIONS { .text : { *(.text) } }\n");
    check(text.find("answer = 42;") != std::string::npos, "recursive SDK expansion");
    check(bool(qld::checkScript(text)), "expanded script parsing");
    auto lua = sdk.lua(R"(
qld.directive('symbol', {'answer', '42'})
qld.macro('code', function(n) qld.section(n, '.text*') end)
qld.entry('_start')
qld.code('.text')
)");
    check(lua.find("answer = 42;") != std::string::npos, "Lua SDK callback");
    check(bool(qld::checkScript(lua)), "Lua generated script parsing");
    for (const char *bad : {"@unknown\n", "@symbol one\n"}) {
        bool failed = false; try { sdk.preprocess(bad); } catch (const std::runtime_error &) { failed = true; }
        check(failed, "invalid SDK invocation accepted");
    }
    sdk.defineMacro("loop", {}, "@loop\n");
    bool failed = false; try { sdk.preprocess("@loop\n"); } catch (const std::runtime_error &) { failed = true; }
    check(failed, "recursive macro limit");
    check(!qld::checkScript("--!lua\nerror('intentional Lua failure')"), "Lua failure lost");
    check(!qld::checkScript("PHDRS { text PT_LOAD; }"), "ignored PHDRS");
    check(!qld::checkScript("OUTPUT_FORMAT(binary)"), "ignored output format");
    check(!qld::checkScript("HIDDEN(secret = 42)"), "ignored hidden visibility");
    check(!qld::checkScript("SECTIONS { .text : { BYTE( } }"), "malformed data directive");
    for (const char *bad : {"x = 1 + ;", "x = -;", "x = 12garbage;", "x = 18446744073709551615K;",
                            "ENTRY(_start) ` ignored", "/* unterminated", "INPUT(\"unterminated)",
                            "SECTIONS { .text : { *(.text) } > }"})
        check(!qld::checkScript(bad), bad);
    std::unique_ptr<qld_linker_t, decltype(&qld_destroy)> linker(qld_create(), qld_destroy);
    const char *params[] = {"value"}; char *error = nullptr, *expanded = nullptr;
    check(!qld_define_macro(linker.get(), "define", params, 1, "answer = ${value};", &error), "C macro definition");
    check(!qld_expand_script(linker.get(), "@define 42", &expanded, &error), "C macro expansion");
    check(std::string(expanded).find("answer = 42;") != std::string::npos, "C macro bytes");
    qld_free_string(expanded);
    check(!qld_register_directive(linker.get(), "custom", [](void *, const char *const *, size_t n,
        qld_source_sink emit, void *sink, char *, size_t) {
      return n ? 1 : emit(sink, "@define 7\n", 10);
    }, nullptr, &error), "C directive registration");
    check(!qld_expand_script(linker.get(), "@custom", &expanded, &error), "C directive expansion");
    check(std::string(expanded).find("answer = 7;") != std::string::npos, "C directive bytes");
    qld_free_string(expanded); qld_free_string(error);
    error = nullptr;
    qld_set_script_text(linker.get(), "@custom\nENTRY(start)\nSECTIONS { .text : { start = .; BYTE(answer) } }");
    uint8_t *image = nullptr; size_t size = 0;
    check(!qld_run_bytes(linker.get(), &image, &size, &error), error ? error : "C SDK link");
    check(size > 4 && image[0] == 0x7f && image[1] == 'E', "C SDK ELF output");
    qld_free_bytes(image, size); qld_free_string(error);
    error = nullptr;
    check(!qld_register_directive(linker.get(), "bad_sink", [](void *, const char *const *, size_t,
        qld_source_sink emit, void *sink, char *, size_t) {
        emit(sink, nullptr, 1); return 0;
    }, nullptr, &error), "register failing sink");
    check(qld_expand_script(linker.get(), "@bad_sink", &expanded, &error) != 0 && !expanded, "sink failure ignored");
    qld_free_string(error);
    std::cout << "linker script SDK regressions passed\n";
    return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
