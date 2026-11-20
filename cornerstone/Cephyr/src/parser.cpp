#include <cephyr/parser.hpp>

#include <cephyr/diagnostics.hpp>
#include <cephyr/preprocess.hpp>

#include <CParser.h>

#include <utility>

namespace cephyr {

struct ParsedUnit::Impl {
    cparser::CLexer lexer;
    cparser::CParser parser;

    explicit Impl(const std::string &text) : lexer(text), parser(&lexer) {}
};

ParsedUnit::ParsedUnit(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ParsedUnit::ParsedUnit(ParsedUnit &&) noexcept = default;
ParsedUnit &ParsedUnit::operator=(ParsedUnit &&) noexcept = default;
ParsedUnit::~ParsedUnit() = default;

cparser::ASTNode *ParsedUnit::root() const {
    return impl_ ? impl_->parser.getAST()->getRoot() : nullptr;
}

std::optional<ParsedUnit> parse(const Preprocessed &source, Diagnostics &diag) {
    const auto initial_errors = diag.error_count();
    auto impl = std::make_unique<ParsedUnit::Impl>(source.text);
    impl->parser.setDiagnosticHandler(
        [&](cparser::DiagnosticKind kind, int line, int column, const char *message) {
            auto location = source.locate(
                line > 0 ? static_cast<unsigned>(line) : 1,
                column > 0 ? static_cast<unsigned>(column) : 0);
            switch (kind) {
            case cparser::DiagnosticKind::LexError:
                return diag.error("cephyr.lex", message, location);
            case cparser::DiagnosticKind::ParseError:
                return diag.error("cephyr.parse", message, location);
            case cparser::DiagnosticKind::SemanticError:
                return diag.error("cephyr.parse.semantic", message, location);
            case cparser::DiagnosticKind::Warning:
                return diag.warning("cephyr.parse.warning", message, location);
            }
        });

    try {
        impl->parser.parse(nullptr);
    } catch (const cparser::ParseFailure &) {
        return std::nullopt;
    }

    impl->parser.setDiagnosticHandler({});
    if (impl->parser.getParsingErrors() != 0 ||
        impl->parser.getAST()->getErrors() != 0 ||
        diag.error_count() != initial_errors)
        return std::nullopt;
    if (!impl->parser.getAST()->getRoot()) {
        diag.error("cephyr.parse", "C parser did not return a translation unit",
                   source.locate(1));
        return std::nullopt;
    }
    return ParsedUnit(std::move(impl));
}

} // namespace cephyr
