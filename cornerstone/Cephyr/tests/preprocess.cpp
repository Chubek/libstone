#include <cephyr/preprocess.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path;

    TemporaryDirectory() {
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("cephyr-preprocess-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream stream(path);
    assert(stream);
    stream << contents;
    assert(stream);
}

unsigned line_containing(const std::string &text, const std::string &needle) {
    const auto position = text.find(needle);
    assert(position != std::string::npos);
    unsigned line = 1;
    for (std::size_t i = 0; i < position; ++i)
        line += text[i] == '\n';
    return line;
}

std::string rendered(const cephyr::Diagnostics &diagnostics) {
    std::ostringstream stream;
    diagnostics.render(stream);
    return stream.str();
}

void check_translation_phases(const std::filesystem::path &directory) {
    const auto source = (directory / "phases.c").string();
    write(directory / "phases.h",
          "#define HEADER_VAL 2?" "?/\r\n8\r"
          "int header_physical_line;\r");
    cephyr::Diagnostics diagnostics;
    auto result = cephyr::preprocess(
        "?" "?=include \"phases.h\"\r\n"
        "#define JOIN(a, b) a %:%: b\r"
        "#define STR(x) %:x\n"
        "const char *tokens = STR(a/**/b);\n"
        "// comment continued by phase 1 then phase 2: ?" "?/\r\n"
        "#error this line is still in the comment\r\n"
        "%:if ?" "?/\n"
        "__LINE__ != 8 || HEADER_VAL != 28\n"
        "#error phases were not applied in order\n"
        "#endif\n"
        "#define PASS(x) x\n"
        "#if PASS(\\\n"
        "__LINE__) != 13\n"
        "#error macro argument lost its physical source line\n"
        "#endif\n"
        "int JOIN(mapped, _token);\n",
        source, {}, diagnostics);
    assert(result);
    assert(diagnostics.error_count() == 0);
    assert(result->text.find("\"a b\"") != std::string::npos);
    const auto header = result->locate(
        line_containing(result->text, "header_physical_line"));
    assert(header.file == (directory / "phases.h").string());
    assert(header.line == 3);
    const auto mapped = result->locate(
        line_containing(result->text, "mapped_token"));
    assert(mapped.file == source);
    assert(mapped.line == 16);

    write(directory / "no-final-newline.h", "int no_final_newline;");
    cephyr::Diagnostics eof;
    result = cephyr::preprocess("#include \"no-final-newline.h\"\n"
                                "int final_splice; ?" "?/\n", source, {}, eof);
    assert(result);
    const auto messages = rendered(eof);
    assert(messages.find("no-final-newline.h:1: warning") != std::string::npos);
    assert(messages.find("phases.c:2: warning") != std::string::npos);
    assert(messages.find("backslash-newline at end of source file") !=
           std::string::npos);

    for (const auto *partial : {"/* unfinished\n", "int c = 'a\n",
                                "const char *s = \"a\n"}) {
        cephyr::Diagnostics malformed;
        assert(!cephyr::preprocess(partial, source, {}, malformed));
        assert(malformed.error_count() >= 1);
    }
}

void check_extended_source(const std::filesystem::path &directory) {
    const auto source = (directory / "unicode.c").string();
    write(directory / "unicode.h",
          "#ifndef G\\u00c9\n#define GÉ\n"
          "#define VA\\U000000E9 41\n#endif\n");
    cephyr::PreprocessOptions options;
    options.macros.emplace_back(true, "\\u0190");
    options.macros.emplace_back(true, "Æ=2");
    options.macros.emplace_back(false, "\\U000000C6");
    cephyr::Diagnostics diagnostics;
    const auto result = cephyr::preprocess(
        "#include \"unicode.h\"\n"
        "#include \"unicode.h\"\n"
        "#define ID(π) \\u03C0\n"
        "#define CAT(a,b) a ## b\n"
        "#define STR(x) #x\n"
        "#define XSTR(x) STR(x)\n"
        "#define f\\u00e9(x) x\n"
        "#if !defined(G\\U000000C9) || ID(CAT(VA, é)) != 41 || Ɛ != 1 || defined(Æ)\n"
        "#error extended macro identity failed\n"
        "#endif\n"
        "#if '\\u00E9' != 'é'\n#error character spellings differ\n#endif\n"
        "#undef VAé\n"
        "#ifdef VA\\u00e9\n#error undef used the wrong lookup key\n#endif\n"
        "STR(\\U000000E9) STR(é) STR(1\\u0301) STR(2é) STR(3\\u00A3) XSTR(f\\U000000E9)\n"
        "XSTR(CAT(other, \\u00E9)) XSTR(CAT(u8, \"é\"))\n"
        "char *s = \"\\u00A3\" \"é\";\n"
        // Form a UCN via a trigraph, then splice in its hex digits using
        // another trigraph and CRLF. A physical UTF-8 sequence cannot be
        // formed by the same splice (checked below).
        "int ?" "?/u00?" "?/\r\nE9;\n",
        source, options, diagnostics);
    assert(result);
    assert(diagnostics.error_count() == 0);
    for (const auto *token : {R"("\U000000E9")", "\"é\"", R"("1\u0301")",
                              "\"2é\"", R"("3\u00A3")", R"("f\U000000E9")", R"("other\u00E9")",
                              "\"u8\\\"é\\\"\"", R"("\u00A3")", R"(\u00E9;)"})
        assert(result->text.find(token) != std::string::npos);

    for (const auto &[text, message] : {
             std::pair{"int \\u0xE9;\n", "malformed universal character name"},
             std::pair{"int x\\U0000E9;\n", "malformed universal character name"},
             std::pair{"int \\u0041;\n", "invalid code point"},
             std::pair{"int a\\uD800;\n", "invalid code point"},
             std::pair{"int \\U00110000;\n", "invalid code point"},
             std::pair{"int \\u0040;\n", "Annex D.1"},
             std::pair{"int x\\u00D7;\n", "Annex D.1"},
             std::pair{"int \\U0001FFFE;\n", "Annex D.1"},
             std::pair{"int x·;\n", ""}, // U+00B7 is permitted.
             std::pair{"int 𝄞;\n", ""},  // U+1D11E is in Annex D.1.
             std::pair{"int £;\n", "Annex D.1"},
             std::pair{"int \\u0301end;\n", "Annex D.2"},
             std::pair{"int \xcc\x81""end;\n", "Annex D.2"},
             std::pair{"#define F(π, \\u03C0) 0\n", "duplicate macro argument"},
             std::pair{"#define F \\u03C0\n#define F π\n", "redefined unidentically"},
             std::pair{"// \xc0\xaf\n", "invalid UTF-8 source encoding"},
             std::pair{"// \xed\xa0\x80\n", "invalid UTF-8 source encoding"},
             std::pair{"// \xf4\x90\x80\x80\n", "invalid UTF-8 source encoding"},
             std::pair{"// \xe2", "truncated UTF-8 source character"},
             std::pair{"int \xc3\\\n\xa9;\n", "invalid UTF-8 source encoding"}}) {
        cephyr::Diagnostics checked;
        const auto output = cephyr::preprocess(text, source, {}, checked);
        if (!*message) {
            assert(output && checked.error_count() == 0);
        } else {
            assert(!output);
            const auto messages = rendered(checked);
            assert(messages.find("unicode.c:") != std::string::npos);
            assert(messages.find(message) != std::string::npos);
        }
    }

    write(directory / "bad-ucn.h",
          "#line 50 \"header-mapped.h\"\r\nint ?" "?/u00?" "?/\r\nA3;\n");
    cephyr::Diagnostics header_errors;
    assert(!cephyr::preprocess("#include \"bad-ucn.h\"\n", source, {}, header_errors));
    assert(rendered(header_errors).find("header-mapped.h:50: error") != std::string::npos);
    assert(rendered(header_errors).find("U+00A3") != std::string::npos);
}

void check_directives(const std::filesystem::path &directory) {
    const auto source = (directory / "directives.c").string();
    write(directory / "direct.h", "int direct_header;\n");
    write(directory / "two words.h", "int stringized_header;\n");
    write(directory / "expanded.h", "int expanded_header;\n");
    cephyr::PreprocessOptions options;
    options.include_dirs.push_back(directory);

    cephyr::Diagnostics diagnostics;
    const auto result = cephyr::preprocess(
        "#define STR(x) #x\n"
        "#define direct nonexistent\n"
        "#include /* directive whitespace */ <direct.h>\n"
        "#include STR(two/**/words.h)\n"
        "#define RIGHT >\n"
        "#include <expanded.h RIGHT\n"
        "#if 1\n"
        "#elif 1 / 0\n"
        "#error in a skipped branch\n"
        "#endif\n"
        "#define HASH #\n"
        "HASH 63 \"not-a-real-marker.c\"\n"
        "#define EMPTY\n"
        "EMPTY %:line 42 \"not-a-directive.c\"\n"
        "int before_remap;\n"
        "#define NUMBER 0000000000000017\n"
        "%:line NUMBER STR(two /* whitespace */ words.c)\n"
        "#if __LINE__ != 17\n"
        "#error incorrect decimal #line interpretation\n"
        "#endif\n"
        "const char *mapped_file = __FILE__;\n"
        "#line 2147483647\n"
        "int largest_line;\n",
        source, options, diagnostics);
    assert(result);
    assert(diagnostics.error_count() == 0);
    for (const auto *token : {"direct_header", "stringized_header",
                              "expanded_header", "\"not-a-real-marker.c\"",
                              "\"not-a-directive.c\"", "\"two words.c\""})
        assert(result->text.find(token) != std::string::npos);
    const auto before = result->locate(
        line_containing(result->text, "before_remap"));
    assert(before.file == source && before.line == 15);
    const auto after = result->locate(
        line_containing(result->text, "mapped_file"));
    assert(after.file == "two words.c" && after.line == 20);
    const auto largest = result->locate(
        line_containing(result->text, "largest_line"));
    assert(largest.file == "two words.c" && largest.line == 2147483647U);

    for (const auto *invalid : {
             "#line 0\n", "#line 2147483648\n",
             "#line 99999999999999999999999999999999\n", "#line 0x20\n",
             "#line 20u\n", "#line 'a'\n", "#line 20 u8\"bad\"\n",
             "#line 20 \"bad\" extra\n", "#include \"direct.h\" extra\n",
             "#define BAD \"direct\" \".h\"\n#include BAD\n",
             "#define BAD u8\"direct.h\"\n#include BAD\n",
             "#if 1\n#else\n#elif 1\n#endif\n",
             "#if 1\n#else\n#else\n#endif\n",
             "#if 1\n#else extra\n#endif\n", "#if 0\n#else extra\n#endif\n",
             "#if 0\n#endif extra\n", "#if 1\n#endif extra\n",
             "#ifdef X extra\n#endif\n", "#ifndef X extra\n#endif\n",
             "#undef X extra\n", "#define X+1\n",
             "#define X 1\n#define X() 1\n", "#define X() 1\n#define X 1\n",
             "#define X(a,b) a\n#define X(a) a\n",
             "#define X(...) 1\n#define X() 1\n",
             "#define X __VA_ARGS__\n", "#define X(__VA_ARGS__) 1\n",
             "int __VA_ARGS__;\n", "#if 0\n#elif __VA_ARGS__\n#endif\n",
             "#define X(...) __VA_ARGS__\nX(1); __VA_ARGS__\n",
             "#undef defined\n", "#undef __STDC_VERSION__\n",
             "#define __STDC_HOSTED__ 1\n", "#undef __STDC_NO_THREADS__\n",
             "#define __STDC_IEC_559__ 1\n", "#define __STDC_UTF_32__ 1\n"}) {
        cephyr::Diagnostics rejected;
        assert(!cephyr::preprocess(invalid, source, options, rejected));
        assert(rejected.error_count() == 1);
        assert(rendered(rejected).find("directives.c:") != std::string::npos);
    }

    cephyr::Diagnostics message;
    assert(!cephyr::preprocess("#define MESSAGE expanded\n"
                               "#error MESSAGE /* omitted */ \"detail\"\n",
                               source, {}, message));
    assert(rendered(message).find("#error MESSAGE \"detail\"") !=
           std::string::npos);

    // ucpp's compact replacement lists cross to a two-byte parameter index
    // after 127; redefining the macro must compare that index without changing it.
    std::string many = "#define MANY(";
    for (unsigned parameter = 0; parameter < 129; ++parameter) {
        if (parameter)
            many += ",";
        many += "p" + std::to_string(parameter);
    }
    many += ") ";
    const std::string definition = many + "p128\n";
    cephyr::Diagnostics identical;
    assert(cephyr::preprocess(definition + definition, source, options, identical));
    cephyr::Diagnostics changed;
    assert(!cephyr::preprocess(definition + many + "p127\n", source, options, changed));
    assert(changed.error_count() == 1);
    assert(rendered(changed).find("redefined unidentically") != std::string::npos);
}

void check_line_filenames() {
    cephyr::Diagnostics diagnostics;
    const auto result = cephyr::preprocess(
        R"(#define WHERE "caf\u00E9-\U0001F642\x2e\143")" "\n"
        "#line 31 WHERE\n"
        "const char *unicode_filename = __FILE__;\n"
        R"(#line 40 "a\"b\\u00E9\?c\n\377")" "\n"
        "const char *byte_filename = __FILE__;\n"
        "#line 60 \"\"\n"
        "const char *empty_filename = __FILE__;\n",
        "line-filenames.c", {}, diagnostics);
    assert(result && rendered(diagnostics).empty());
    const auto verify_filenames = [](const cephyr::Preprocessed &mapped) {
        auto at = mapped.locate(line_containing(mapped.text, "unicode_filename"));
        assert(at.file == "café-🙂.c" && at.line == 31);
        at = mapped.locate(line_containing(mapped.text, "byte_filename"));
        assert(at.file == "a\"b\\u00E9?c\n\xff" && at.line == 40);
        at = mapped.locate(line_containing(mapped.text, "empty_filename"));
        assert(at.file.empty() && at.line == 60);
        assert(mapped.text.find("\"café-🙂.c\"") != std::string::npos);
        assert(mapped.text.find(R"("a\"b\\u00E9\?c\012\377")") != std::string::npos);
        assert(mapped.text.find(R"("")") != std::string::npos);
    };
    verify_filenames(*result);

    cephyr::Diagnostics raw;
    const auto round_trip = cephyr::read_preprocessed(result->output, "line-filenames.i", raw);
    assert(round_trip && rendered(raw).empty());
    assert(round_trip->output == result->output);
    verify_filenames(*round_trip);

    cephyr::Diagnostics truncated;
    const auto nul = cephyr::preprocess(
        R"(#line 10 "presumed\0unused")" "\nint nul_name;\n",
        "line-filenames.c", {}, truncated);
    assert(nul && truncated.error_count() == 0);
    assert(rendered(truncated).find("NUL byte truncates #line filename") != std::string::npos);
    assert(nul->locate(line_containing(nul->text, "nul_name")).file == "presumed");

    for (const auto &[filename, message] : {
             std::pair{R"("\q")", "invalid escape sequence"},
             std::pair{R"("\x")", "hexadecimal escape requires a digit"},
             std::pair{R"("\400")", "numeric escape out of range"},
             std::pair{R"("\x1000000000000000000000")", "numeric escape out of range"},
             std::pair{R"("\u123")", "malformed universal character name"},
             std::pair{R"("\U00110000")", "invalid code point"},
             std::pair{R"("\uD800")", "invalid code point"},
             std::pair{R"("\u0041")", "invalid code point"},
             std::pair{R"("\0\q")", "invalid escape sequence"}}) {
        cephyr::Diagnostics rejected;
        assert(!cephyr::preprocess(
            std::string(R"(#line 70 "pr\u00E9vious.c")") +
                "\n#define BAD_NAME " + filename + "\n#line 100 BAD_NAME\n",
            "line-filenames.c", {}, rejected));
        assert(rejected.error_count() == 1);
        const auto messages = rendered(rejected);
        assert(messages.find("prévious.c:71: error") != std::string::npos);
        assert(messages.find(std::string(message) + " in ") != std::string::npos);
        assert(messages.find("#line filename") != std::string::npos);
    }

    cephyr::PreprocessOptions assembler;
    assembler.assembler = true;
    cephyr::Diagnostics assembly;
    const auto unchanged = cephyr::preprocess(
        R"(#line 10 "assembler\q.c")" "\n"
        "#if 'ab' == 'a'\n.byte 'ab'\n#endif\n",
        "line-filenames.S", assembler, assembly);
    assert(unchanged && assembly.error_count() == 0);
    assert(unchanged->locate(line_containing(unchanged->text, ".byte")).file == "assemblerq.c");
}

void check_conditional_expressions() {
    cephyr::Diagnostics diagnostics;
    const auto result = cephyr::preprocess(
        "#if 0\n"
        "#ifdef bad tokens\n"
        "#else bad tokens\n"
        "#endif bad tokens\n"
        "#define BAD __VA_ARGS__\n"
        "#endif\n"
        "#define V(head, ...) head __VA_ARGS__\n"
        "#define V(head,...) /* ignored */ head __VA_ARGS__ /* ignored */\n"
        "#if (1 ? -1 : 1U / 0) != 18446744073709551615U\n"
        "#error an unselected operand still contributes its signedness\n"
        "#endif\n"
        "#if (0 ? 1U << 64 : -1) < 0 || (1 ? -1 : (1 ? 1U : 0)) < 0\n"
        "#error the conditional type depends on both arms\n"
        "#endif\n"
        "#if (1 ? -1 : !1U) >= 0 || (1 ? -1 : 1 << 0U) >= 0\n"
        "#error logical negation yields int and a shift has its left operand type\n"
        "#endif\n"
        "#if !(1 || (1, 2)) || (0 ? 1, 2 : 3) != 3 || (1 ? 3 : (1, 2)) != 3\n"
        "#error an unevaluated comma operator is permitted\n"
        "#endif\n"
        "#if (1 ? 2 : -(-9223372036854775807 - 1)) != 2 || (-3 >> 1) != -2\n"
        "#error dead arithmetic must not be performed\n"
        "#endif\n"
        "#if 0x8000000000000000 < 0 || -1U != 18446744073709551615U\n"
        "#error expected 64-bit phase-4 integer types\n"
        "#endif\n"
        "#if !1U || !(1U && 2) || !(1 && 2U)\n"
        "#error logical operators work across signedness\n"
        "#endif\n"
        "#if '\\377' != 255 || u'\\xffff' != 65535 || U'\\xffffffff' != 4294967295U\n"
        "#error maximum numeric escape values are valid\n"
        "#endif\n"
        R"(#if 'é' != 0xc3a9 || '\u00e9' != 0xc3a9 || u'🙂' != 0xde42)" "\n"
        "#error wrong UTF-8 or UTF-16 execution conversion\n"
        "#endif\n"
        R"(#if L'\xffffffff' != -1 || L'a' - L'b' >= 0 || U'\U0001F642' != 0x1f642)" "\n"
        "#error wrong default wide character conversion\n"
        "#endif\n"
        "int V(value, );\n",
        "if-expressions.c", {}, diagnostics);
    assert(result);
    assert(rendered(diagnostics).empty());
    assert(result->text.find("value") != std::string::npos);

    for (const auto &[expression, message] : {
             std::pair{"(1, 2)", "evaluated comma operator"},
             std::pair{"1 ? 1, 2 : 3", "evaluated comma operator"},
             std::pair{"(1U << 64)", "shift count out of range"},
             std::pair{"(1 >> -1)", "shift count out of range"},
             std::pair{"1 << 63", "signed overflow"},
             std::pair{"9223372036854775807 + 1", "signed overflow"},
             std::pair{"-9223372036854775807 - 2", "signed overflow"},
             std::pair{"9223372036854775807 * 2", "signed overflow"},
             std::pair{"-(-9223372036854775807 - 1)", "overflow on unary minus"},
             std::pair{"(-9223372036854775807 - 1) % -1", "overflow on division"},
             std::pair{"1 || 0xU", "invalid integer constant"},
             std::pair{"'\\x'", "hexadecimal escape requires a digit"},
             std::pair{"'a\\q'", "invalid escape sequence"},
              std::pair{"'\\400'", "numeric escape out of range"},
              std::pair{"u'\\x10000'", "numeric escape out of range"},
              std::pair{"L'\\x10000000000000000000000000'", "numeric escape out of range"},
             std::pair{"U'\\x100000000'", "numeric escape out of range"}}) {
        cephyr::Diagnostics rejected;
        assert(!cephyr::preprocess(std::string("#if ") + expression + "\n#endif\n",
                                   "if-expressions.c", {}, rejected));
        assert(rejected.error_count() == 1);
        assert(rendered(rejected).find(message) != std::string::npos);
    }
}

void check_expanded_literals(const std::filesystem::path &directory) {
    const auto source = (directory / "expanded-literals.c").string();
    write(directory / "expanded-literals.h",
          "#line 40 \"literals-mapped.h\"\n"
          "#define WIDE U\"wide\"\n"
          "void *s = u8\"UTF-8\" \"ordinary\"\n"
          "/* still adjacent */ WIDE;\n");

    cephyr::Diagnostics mixed;
    assert(!cephyr::preprocess("#include \"expanded-literals.h\"\n",
                               source, {}, mixed));
    assert(mixed.error_count() == 1);
    assert(rendered(mixed).find("literals-mapped.h:42: error") != std::string::npos);
    assert(rendered(mixed).find("cannot mix a UTF-8 prefix with a wide prefix") !=
           std::string::npos);

    for (const auto &[text, message] : {
             std::pair{R"(void *p = L"wide" u8"utf8";)", "cannot mix a UTF-8 prefix"},
             std::pair{R"(void *p = u"wide" "" u8"utf8";)", "cannot mix a UTF-8 prefix"},
             std::pair{R"(void *p = u8"utf8" "" u"wide";)", "cannot mix a UTF-8 prefix"},
             std::pair{R"(char *s = "\q";)", "invalid escape sequence"},
             std::pair{R"(char *s = "\x";)", "hexadecimal escape requires a digit"},
             std::pair{R"(char *s = "\400";)", "numeric escape out of range"},
             std::pair{R"(char *s = u8"\x100";)", "numeric escape out of range"},
             std::pair{R"(void *s = u"\x10000";)", "numeric escape out of range"},
             std::pair{R"(void *s = U"\x100000000";)", "numeric escape out of range"},
             std::pair{R"(int c = L'\x100000000';)", "numeric escape out of range"},
             std::pair{R"(int c = '\8';)", "invalid escape sequence"},
             std::pair{R"(int c = '';)", "empty character constant"},
             std::pair{R"(char *s = "\u12x4";)", "malformed universal character name"},
             std::pair{R"(char *s = "\u0041";)", "invalid code point"},
             std::pair{R"(char *s = "\U00110000";)", "invalid code point"}}) {
        cephyr::Diagnostics rejected;
        assert(!cephyr::preprocess(std::string(text) + '\n', source, {}, rejected));
        assert(rejected.error_count() == 1);
        assert(rendered(rejected).find(message) != std::string::npos);
    }

    cephyr::Diagnostics valid;
    cephyr::PreprocessOptions comments;
    comments.keep_comments = true;
    auto result = cephyr::preprocess(
        "#define TWICE(x) x x\n"
        "#if 0\nchar *skipped = \"\\q\";\n#endif\n"
        "// \"\\x100\" u8\"utf8\" L\"wide\" is a comment\n"
        R"(char *s = u8"\377\x000ff\u0040\\u0041" /* "\q" */ "tail";)" "\n"
        R"(char *hex_boundary = "\x12" "3";)" "\n"
        R"(void *w = TWICE(u"\xffff") "ordinary";)" "\n"
        R"(void *v = U"\xffffffff";)" "\n"
        R"(int wch = L'\xffffffff';)" "\n",
        source, comments, valid);
    assert(result && rendered(valid).empty());
    assert(result->text.find(R"("\x12" "3")") != std::string::npos);
    assert(result->text.find(R"(U"\xffffffff")") != std::string::npos);

    cephyr::Diagnostics preprocessed;
    assert(!cephyr::read_preprocessed(
        "# 70 \"literals-input.c\" 1\r\n"
        "void *w = u8\"utf8\" /* ordinary comment */\r\n"
        "L\"wide\";\r\n", "literals.i", preprocessed));
    assert(preprocessed.error_count() == 1);
    assert(rendered(preprocessed).find("literals-input.c:71: error") !=
           std::string::npos);
}

void check_pragmas() {
    cephyr::Diagnostics diagnostics;
    const auto result = cephyr::preprocess(
        "#pragma unknown \"C11 ignores this\"\n"
        "#pragma STDC FP_CONTRACT /* no contraction */ OFF\n"
        "#define STDC not_expanded\n"
        "#define OFF not_expanded\n"
        "#pragma STDC FENV_ACCESS OFF\n"
        "#define DO_PRAGMA(x) _Pragma(#x)\n"
        "_Pragma(u8\"STDC CX_LIMITED_RANGE /* comment */ DEFAULT\")\n"
        "_Pragma(\"unknown \\\"quoted\\\" \\\\path\")\n"
        "_Pragma(\"unknown // trailing comment\")\n"
        "int n = DO_PRAGMA(unknown) 42;\n",
        "pragmas.c", {}, diagnostics);
    assert(result);
    assert(diagnostics.error_count() == 0);
    assert(rendered(diagnostics).empty());
    assert(result->output.find("#pragma") == std::string::npos);
    assert(result->output.find("_Pragma") == std::string::npos);
    assert(result->text.find("42") != std::string::npos);
    assert(result->locate(line_containing(result->text, "int n")).line == 10);

    for (const auto *invalid : {
             "#pragma STDC FP_CONTRACT YES\n",
             "#define MODE OFF\n#pragma STDC FP_CONTRACT MODE\n",
             "_Pragma(\"STDC FENV_ACCESS ON\")\n",
             "_Pragma(\"STDC FP_CONTRACT OFF extra\")\n",
             "_Pragma(\"STDC FP_CONTRACT /* unterminated\")\n",
             "_Pragma(1)\n"}) {
        cephyr::Diagnostics rejected;
        assert(!cephyr::preprocess(invalid, "pragmas.c", {}, rejected));
        assert(rejected.error_count() == 1);
        assert(rendered(rejected).find("pragmas.c:") != std::string::npos);
    }
}

} // namespace

int main() {
    TemporaryDirectory temporary;
    check_translation_phases(temporary.path);
    check_extended_source(temporary.path);
    check_directives(temporary.path);
    check_line_filenames();
    check_conditional_expressions();
    check_expanded_literals(temporary.path);
    check_pragmas();
    write(temporary.path / "local.h", "int from_header = VALUE;\n");
    write(temporary.path / "forced.h", "#define FORCED 9\n");

    cephyr::PreprocessOptions options;
    options.include_dirs.push_back(temporary.path);
    options.forced_includes.push_back("forced.h");
    options.macros.emplace_back(true, "VALUE=7");
    options.macros.emplace_back(true, "REMOVED=1");
    options.macros.emplace_back(false, "REMOVED");

    cephyr::Diagnostics diagnostics;
    const auto primary = temporary.path / "main.c";
    auto result = cephyr::preprocess(
        "#include \"local.h\"\n"
        "const char *line_string = \"#line 80 \\\"not-a-marker\\\"\";\n"
        "int source_value = FORCED;\n"
        "#ifdef REMOVED\n"
        "#error -D/-U ordering was lost\n"
        "#endif\n"
        "#if __STDC_VERSION__ != 201112L\n"
        "#error wrong language version\n"
        "#endif\n"
        "#if __STDC_HOSTED__ != 1\n"
        "#error wrong hosted implementation indicator\n"
        "#endif\n"
        "#if __STDC_NO_ATOMICS__ != 1 || __STDC_NO_COMPLEX__ != 1 || "
        "__STDC_NO_THREADS__ != 1 || __STDC_NO_VLA__ != 1\n"
        "#error wrong C11 feature indicators\n"
        "#endif\n"
        "#if __STDC_UTF_16__ != 1 || __STDC_UTF_32__ != 1\n"
        "#error wrong Unicode encoding indicators\n"
        "#endif\n"
        "#if defined(__STDC_IEC_559__)\n"
        "#error unsupported optional feature was advertised\n"
        "#endif\n",
        primary.string(), options, diagnostics);
    assert(result);
    assert(diagnostics.error_count() == 0);
    assert(result->text.find("from_header") != std::string::npos);
    assert(result->text.find("source_value") != std::string::npos);
    assert(result->text.find("#line 80") != std::string::npos);
    assert(result->output.find("# 1 \"") != std::string::npos);

    const auto header_location =
        result->locate(line_containing(result->text, "from_header"));
    assert(std::filesystem::path(header_location.file).filename() == "local.h");
    assert(header_location.line == 1);

    const auto source_location =
        result->locate(line_containing(result->text, "source_value"));
    assert(source_location.file == primary.string());
    assert(source_location.line == 3);

    const auto include_path = temporary.path / "include";
    std::filesystem::create_directory(include_path);
    write(include_path / "sibling.h", "#define ANGLE_VALUE 12\n");
    write(temporary.path / "sibling.h",
          "#line 80 \"virtual\\\\header.h\"\n"
          "const char *inner_file = __FILE__;\n"
          "int local_winner = ANGLE_VALUE;\n");
    cephyr::PreprocessOptions relative;
    relative.include_dirs.push_back(include_path);
    cephyr::Diagnostics relative_diagnostics;
    const auto primary_relative = std::filesystem::relative(primary);
    auto relative_result = cephyr::preprocess(
        "#include <sibling.h>\r\n"
        "#line 50 \"different-dir/mapped.c\"\r\n"
        "#include \"sibling.h\"\r\n"
        "const char *outer_file = __FILE__;\r\n",
        primary_relative.string(), relative, relative_diagnostics);
    assert(relative_result);
    const auto local_winner = relative_result->locate(
        line_containing(relative_result->text, "local_winner"));
    assert(local_winner.file == "virtual\\header.h");
    assert(local_winner.line == 81);
    const auto outer_file = relative_result->locate(
        line_containing(relative_result->text, "outer_file"));
    assert(outer_file.file == "different-dir/mapped.c");
    assert(outer_file.line == 51);
    assert(relative_result->text.find("\"virtual\\\\header.h\"") !=
           std::string::npos);
    assert(relative_result->text.find("\"different-dir/mapped.c\"") !=
           std::string::npos);

    write(temporary.path / "diagnostic.h",
          "#line 45 \"virtual/error.h\"\n#error mapped diagnostic\n");
    cephyr::Diagnostics relative_error_diagnostics;
    const auto relative_error = cephyr::preprocess(
        "#include \"diagnostic.h\"\n", primary_relative.string(), {},
        relative_error_diagnostics);
    assert(!relative_error);
    std::ostringstream relative_error_text;
    relative_error_diagnostics.render(relative_error_text);
    assert(relative_error_text.str().find("virtual/error.h:45:") !=
           std::string::npos);

    cephyr::Diagnostics escaped_diagnostics;
    const std::string escaped_file = "dir\"name\\with-question?.c";
    const auto escaped =
        cephyr::preprocess("const char *filename = __FILE__;\n", escaped_file,
                           {}, escaped_diagnostics);
    assert(escaped);
    assert(escaped->locate(1).file == escaped_file);
    assert(escaped->text.find("dir\\\"name\\\\with-question\\?.c") !=
           std::string::npos);

    cephyr::Diagnostics literal_diagnostics;
    const auto literals = cephyr::preprocess(
        "#define u8 must_not_expand\n"
        "#define u must_not_expand\n"
        "#define U must_not_expand\n"
         "#define TEXT u8\"text\"\n"
         "#if u'A' != 'A' || U'B' != 'B'\n#error bad C11 character token\n#endif\n"
         "#if '\\u0040' != '@'\n#error bad universal character name value\n#endif\n"
        "const void *values[] = {TEXT, u\"wide16\", U\"wide32\", L\"wide\"};\n",
        primary.string(), {}, literal_diagnostics);
    assert(literals);
    assert(literals->text.find("must_not_expand") == std::string::npos);
    for (const std::string literal :
         {"u8\"text\"", "u\"wide16\"", "U\"wide32\"", "L\"wide\""})
         assert(literals->text.find(literal) != std::string::npos);

    for (const auto *condition : {"'\\u0041'", "u'\\udfff'", "U'\\U00110000'"}) {
        cephyr::Diagnostics bad_ucn_diagnostics;
        assert(!cephyr::preprocess(
            std::string("#if ") + condition + "\n#endif\n",
            primary.string(), {}, bad_ucn_diagnostics));
        std::ostringstream out;
        bad_ucn_diagnostics.render(out);
        assert(out.str().find("invalid code point in UCN") != std::string::npos);
    }

    cephyr::PreprocessOptions comments;
    comments.keep_comments = true;
    comments.line_markers = false;
    cephyr::Diagnostics comment_diagnostics;
    auto comment_result =
        cephyr::preprocess("/* prefix */ #line 45 \"comment-mapped.c\"\n"
                           "int x /* retained */ = 1;\n", primary.string(),
                           comments, comment_diagnostics);
    assert(comment_result);
    assert(comment_result->output == comment_result->text);
    assert(comment_result->text.find("/* prefix */") != std::string::npos);
    assert(comment_result->text.find("/* retained */") != std::string::npos);
    assert(comment_result->output.find("# ") == std::string::npos);
    const auto comment_mapped = comment_result->locate(
        line_containing(comment_result->text, "int x"));
    assert(comment_mapped.file == "comment-mapped.c");
    assert(comment_mapped.line == 45);

    cephyr::PreprocessOptions trigraphs;
    cephyr::Diagnostics trigraph_diagnostics;
    const std::string trigraph_source =
        "?" "?=define TRIGRAPH_VALUE 11\nint trigraph = TRIGRAPH_VALUE;\n";
    auto trigraph_result = cephyr::preprocess(
        trigraph_source, primary.string(), trigraphs, trigraph_diagnostics);
    assert(trigraph_result);
    assert(trigraph_result->text.find("TRIGRAPH_VALUE") == std::string::npos);
    assert(trigraph_result->text.find("11") != std::string::npos);

    trigraphs.trigraphs = false;
    cephyr::Diagnostics disabled_trigraph_diagnostics;
    auto disabled_trigraph_result = cephyr::preprocess(
        trigraph_source, primary.string(), trigraphs,
        disabled_trigraph_diagnostics);
    assert(disabled_trigraph_result);
    assert(disabled_trigraph_result->text.find("TRIGRAPH_VALUE") !=
           std::string::npos);

    cephyr::PreprocessOptions freestanding;
    freestanding.hosted = false;
    cephyr::Diagnostics freestanding_diagnostics;
    auto freestanding_result = cephyr::preprocess(
        "#if __STDC_VERSION__ != 201112L\n#error wrong C version\n#endif\n"
        "#if __STDC_HOSTED__ != 0\n#error hosted macro was redefined\n#endif\n"
        "int freestanding_translation_unit;\n",
        primary.string(), freestanding, freestanding_diagnostics);
    assert(freestanding_result);

    cephyr::Diagnostics preprocessed_diagnostics;
    const std::string marked_input =
        "# 20 \"mapped.c\" 1 3\r\n"
        "const char *marker = \"# 3 \\\"still-a-string\\\"\";\r\n"
        "/* comment\r\n"
        "# 500 \"not-a-marker.c\"\r\n"
        "end of comment */\r\n"
        "#line 100\r\n"
        "int mapped_again;\r\n";
    auto already_preprocessed =
        cephyr::read_preprocessed(marked_input, "input.i", preprocessed_diagnostics);
    assert(already_preprocessed);
    assert(already_preprocessed->output == marked_input);
    assert(already_preprocessed->text.find("# 3") != std::string::npos);
    assert(already_preprocessed->text.find("# 500 \"not-a-marker.c\"") !=
           std::string::npos);
    const auto mapped = already_preprocessed->locate(1, 4);
    assert(mapped.file == "mapped.c");
    assert(mapped.line == 20);
    assert(mapped.column == 4);
    const auto mapped_again = already_preprocessed->locate(
        line_containing(already_preprocessed->text, "mapped_again"));
    assert(mapped_again.file == "mapped.c");
    assert(mapped_again.line == 100);

    write(temporary.path / "local.inc", "#define ASSEMBLY_VALUE 37\n");
    cephyr::PreprocessOptions assembler;
    assembler.assembler = true;
    assembler.line_markers = false;
    const std::string assembly =
        "# ordinary assembler comment with /* unmatched punctuation\n"
        "# 3 is an assembler comment, too\n"
        "# 'or an unmatched apostrophe\n"
        "#include \"local.inc\"\n"
        "#if 0\n"
        "# this comment is compiled out\n"
        "#endif\n"
        ".macro set_value reg\n"
        "  li \\reg, ASSEMBLY_VALUE # don't interpret the quote\n"
        ".endm\n";
    cephyr::Diagnostics assembler_diagnostics;
    const auto assembly_result =
        cephyr::preprocess(assembly, (temporary.path / "input.S").string(),
                           assembler, assembler_diagnostics);
    assert(assembly_result);
    assert(assembly_result->text.find("# ordinary assembler comment") !=
           std::string::npos);
    assert(assembly_result->text.find("# 3 is an assembler comment") !=
           std::string::npos);
    assert(assembly_result->text.find("# 'or an unmatched apostrophe") !=
           std::string::npos);
    assert(assembly_result->text.find("compiled out") == std::string::npos);
    assert(assembly_result->text.find("\\reg,") != std::string::npos);
    assert(assembly_result->text.find("37") != std::string::npos);
    assert(assembly_result->text.find("don't interpret") != std::string::npos);

    cephyr::Diagnostics error_diagnostics;
    auto error =
        cephyr::preprocess("#error expected\n", "bad.c", {}, error_diagnostics);
    assert(!error);
    assert(error_diagnostics.error_count() == 1);

    cephyr::Diagnostics isolated_diagnostics;
    auto isolated = cephyr::preprocess(
        "#ifdef VALUE\n#error macro leaked from a previous translation unit\n#endif\n"
        "int clean_after_error;\n",
        "isolated.c", {}, isolated_diagnostics);
    assert(isolated);
    assert(isolated->text.find("clean_after_error") != std::string::npos);
}
