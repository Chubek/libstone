#include <cephyr/diagnostics.hpp>

#include <cassert>
#include <sstream>
#include <string>

int main() {
    cephyr::Diagnostics diagnostics;
    diagnostics.warning("C100", "a warning");
    diagnostics.error("C200", "an error", {"source.ce", 4, 2});
    diagnostics.note("", "context", {"relative file.ce"});

    assert(diagnostics.error_count() == 1);
    assert(!diagnostics.warnings_as_errors());
    diagnostics.set_warnings_as_errors(true);
    assert(diagnostics.error_count() == 2);
    assert(diagnostics.warnings_as_errors());

    std::ostringstream text;
    diagnostics.render(text);
    assert(text.str().find("source.ce:4:2: error[C200]: an error") != std::string::npos);
    assert(text.str().find("error[C100]: a warning") != std::string::npos);
    assert(text.str().find(":1:1") == std::string::npos);
    diagnostics.set_warnings_as_errors(false);
    assert(diagnostics.error_count() == 1);

    cephyr::Diagnostics path_only;
    path_only.error("C300", "path only", {"relative file.ce", 0, 8});
    std::ostringstream sarif;
    path_only.render(sarif, true);
    assert(sarif.str().find("\"version\":\"2.1.0\"") != std::string::npos);
    assert(sarif.str().find("\"uri\":\"relative%20file.ce\"") != std::string::npos);
    assert(sarif.str().find("\"region\"") == std::string::npos);

    cephyr::Diagnostics special_path;
    special_path.error("C301", "special path", {"literal%20 #?.ce", 9, 6});
    std::ostringstream special_sarif;
    special_path.render(special_sarif, true);
    assert(special_sarif.str().find(
               "\"uri\":\"literal%2520%20%23%3F.ce\"") != std::string::npos);
    assert(special_sarif.str().find(
               "\"region\":{\"startLine\":9,\"startColumn\":6}") != std::string::npos);

    cephyr::Diagnostics mapped_line;
    mapped_line.warning("C302", "mapped line", {"file:tag.ce", 21});
    std::ostringstream mapped_sarif;
    mapped_sarif << std::hex;
    mapped_line.render(mapped_sarif, true);
    assert(mapped_sarif.str().find("\"level\":\"warning\"") != std::string::npos);
    assert(mapped_sarif.str().find("\"uri\":\"file%3Atag.ce\"") != std::string::npos);
    assert(mapped_sarif.str().find("\"region\":{\"startLine\":21}") != std::string::npos);
    assert(mapped_sarif.str().find("\"startColumn\"") == std::string::npos);

    cephyr::Diagnostics unknown;
    unknown.error("C400", "no source");
    std::ostringstream unknown_sarif;
    unknown.render(unknown_sarif, true);
    assert(unknown_sarif.str().find("\"locations\"") == std::string::npos);

    cephyr::Diagnostics unsafe_text;
    unsafe_text.error("C500", "first line\nsecond line");
    std::ostringstream escaped;
    unsafe_text.render(escaped);
    assert(escaped.str() == "error[C500]: first line\\nsecond line\n");

    cephyr::Diagnostics uncoded;
    uncoded.note("", "uncoded note", {"note.ce", 7, 4});
    assert(uncoded.error_count() == 0);
    std::ostringstream uncoded_sarif;
    uncoded.render(uncoded_sarif, true);
    assert(uncoded_sarif.str().find("\"ruleId\"") == std::string::npos);
    assert(uncoded_sarif.str().find(
               "\"region\":{\"startLine\":7,\"startColumn\":4}") != std::string::npos);

    cephyr::Diagnostics bytes;
    bytes.error("C600", "valid \xC3\xA9 invalid \xFF newline\n");
    std::ostringstream byte_sarif;
    bytes.render(byte_sarif, true);
    assert(byte_sarif.str().find("valid \xC3\xA9 invalid \\\\xFF newline\\n") !=
           std::string::npos);
}
