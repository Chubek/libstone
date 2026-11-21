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
    check_directives(temporary.path);
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
        "#if defined(__STDC_IEC_559__) || defined(__STDC_UTF_32__)\n"
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
