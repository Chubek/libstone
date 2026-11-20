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

} // namespace

int main() {
    TemporaryDirectory temporary;
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
        "const void *values[] = {TEXT, u\"wide16\", U\"wide32\", L\"wide\"};\n",
        primary.string(), {}, literal_diagnostics);
    assert(literals);
    assert(literals->text.find("must_not_expand") == std::string::npos);
    for (const std::string literal :
         {"u8\"text\"", "u\"wide16\"", "U\"wide32\"", "L\"wide\""})
        assert(literals->text.find(literal) != std::string::npos);

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
