#pragma once

#include "DiagnosticsCore.hpp"
#include "LLTableDriven.hpp"
#include "LexerGenerator.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace firestone::lr {

struct LRByteSpan {
    std::size_t begin, end; // Half-open byte range in the working source.
};

struct LRResourceManager {
    // Invoked once per fatal result, never on success or exception unwinding.
    std::function<void()> cleanup;
};

struct LRRecoveryOptions {
    std::size_t maxPatches = 16; // Shared across lexing and parsing.
    LRResourceManager resources;
};

enum class LRRecoveryFailure {
    diagnostic_error,
    unpatched,
    no_progress,
    patch_limit,
    invalid_parse_position
};

struct LRRecoveryDiagnostic {
    LRDiagPhase phase;
    LRByteSpan bytes; // Coordinates at diagnosis time, before any ensuing edit.
    std::size_t tokenPosition;
    std::optional<Symbol> found;
    std::vector<Symbol> expected;
    LRDiagAction action;
    bool applied = false;
};

struct LRLexRecoveryResult {
    std::string source;
    std::string provenance;
    std::vector<Symbol> tokens;
    // Parallel to tokens; no EOF symbol is stored. EOF maps to source.size().
    std::vector<LRByteSpan> tokenBytes;
    std::vector<LRRecoveryDiagnostic> diagnostics;
    std::optional<LRRecoveryFailure> fatal;

    bool ok() const noexcept { return !fatal; }
};

struct LRParseRecoveryResult : LRLexRecoveryResult {
    // Set on success. ParseNode offsets index the final tokens, not the source.
    std::optional<ParseNode> tree;
};

using LRParseAttempt = std::variant<ParseNode, LLSyntaxError>;
using LRParseCallback = std::function<LRParseAttempt(const std::vector<Symbol>&)>;

namespace detail {

class LRRecoveryRun {
public:
    LRRecoveryRun(LRLexRecoveryResult& result, std::string_view source,
                  std::string_view provenance, const LRDiagEngine& engine,
                  const LRRecoveryOptions& options, const Grammar* grammar)
        : result_(result), engine_(engine), options_(options), grammar_(grammar) {
        result_.source = source;
        result_.provenance = provenance;
    }

    enum class Step { skip, retry, fatal };

    bool lex(const Lexer& lexer) {
        for (;;) {
            result_.tokens.clear();
            result_.tokenBytes.clear();
            std::size_t offset = 0;
            for (;;) {
                const auto token = lexer.next(result_.source, offset);
                if (token.status == LexStatus::End) return true;
                const LRByteSpan bytes{token.offset, token.offset + token.length};
                if (token.status == LexStatus::Token) {
                    result_.tokens.push_back(token.token);
                    result_.tokenBytes.push_back(bytes);
                    continue;
                }
                const auto step = diagnose(LRDiagPhase::lexical, bytes, result_.tokens.size());
                if (step == Step::fatal) return false;
                if (step == Step::retry) break; // A change can affect any earlier match.
                // The lexer already consumed the invalid byte. Leaving it in
                // source (as a separator) preserves the spans of other tokens.
            }
        }
    }

    Step syntaxError(const LLSyntaxError& error) {
        if (error.position > result_.tokens.size())
            return fail(LRRecoveryFailure::invalid_parse_position);
        const auto bytes = error.position == result_.tokens.size()
            ? LRByteSpan{result_.source.size(), result_.source.size()}
            : result_.tokenBytes[error.position];
        return diagnose(LRDiagPhase::syntactic, bytes, error.position, error.found, error.expected);
    }

private:
    Step fail(LRRecoveryFailure reason) {
        result_.fatal = reason;
        if (options_.resources.cleanup) options_.resources.cleanup();
        return Step::fatal;
    }

    Step diagnose(LRDiagPhase phase, LRByteSpan bytes, std::size_t pos,
                  std::optional<Symbol> found = {}, std::vector<Symbol> expected = {}) {
        LRDiagState state{phase, result_.source, result_.provenance, bytes.begin, bytes.end,
                          grammar_, &result_.tokens, pos, found, std::move(expected)};
        auto action = engine_.diagnose(state);
        auto& entry = result_.diagnostics.emplace_back(LRRecoveryDiagnostic{
            phase, bytes, pos, found, std::move(state.expected), std::move(action)});
        if (entry.action.severity == LRDiagSeverity::error)
            return fail(LRRecoveryFailure::diagnostic_error);
        if (!entry.action.patch) {
            if (phase == LRDiagPhase::lexical &&
                (entry.action.severity == LRDiagSeverity::warning ||
                 entry.action.severity == LRDiagSeverity::inform))
                return Step::skip;
            return fail(LRRecoveryFailure::unpatched);
        }
        const auto& patch = *entry.action.patch;
        // The engine has validated the patch's phase, range and token IDs.
        const auto step = std::visit([&](const auto& edit) { return apply(edit); }, patch);
        if (step == Step::retry) {
            ++patches_;
            entry.applied = true;
        }
        return step;
    }

    Step apply(const LRBytePatch& edit) {
        auto& source = result_.source;
        if (source.compare(edit.begin, edit.end - edit.begin, edit.replacement) == 0)
            return fail(LRRecoveryFailure::no_progress);
        if (patches_ >= options_.maxPatches) return fail(LRRecoveryFailure::patch_limit);
        source.replace(edit.begin, edit.end - edit.begin, edit.replacement);
        return Step::retry;
    }

    Step apply(const LRTokenPatch& edit) {
        auto& tokens = result_.tokens;
        auto& bytes = result_.tokenBytes;
        using TokenOffset = std::vector<Symbol>::difference_type;
        const auto begin = tokens.cbegin() + static_cast<TokenOffset>(edit.begin);
        const auto end = tokens.cbegin() + static_cast<TokenOffset>(edit.end);
        if (std::equal(begin, end, edit.replacement.begin(), edit.replacement.end()))
            return fail(LRRecoveryFailure::no_progress);
        if (patches_ >= options_.maxPatches) return fail(LRRecoveryFailure::patch_limit);
        const auto anchor = edit.begin == tokens.size() ? result_.source.size() : bytes[edit.begin].begin;
        tokens.insert(tokens.erase(begin, end), edit.replacement.begin(), edit.replacement.end());
        using ByteOffset = std::vector<LRByteSpan>::difference_type;
        const auto beginBytes = bytes.cbegin() + static_cast<ByteOffset>(edit.begin);
        const auto endBytes = bytes.cbegin() + static_cast<ByteOffset>(edit.end);
        bytes.insert(bytes.erase(beginBytes, endBytes), edit.replacement.size(), {anchor, anchor});
        return Step::retry;
    }

    LRLexRecoveryResult& result_;
    const LRDiagEngine& engine_;
    const LRRecoveryOptions& options_;
    const Grammar* grammar_;
    std::size_t patches_ = 0;
};

} // namespace detail

// Both wrappers borrow their lexer and engine. Lexer IDs are grammar Symbols
// when used for parsing. No caller-owned input is edited. Exceptions from the
// parse and cleanup callbacks propagate; the engine handles its own callbacks.
// State views handed to the engine are valid only during diagnose(); lexical
// diagnostics have the scanned token prefix, but no found or expected symbol.
class LRPatchLexer {
public:
    LRPatchLexer(const Lexer& lexer, const LRDiagEngine& engine, LRRecoveryOptions options = {})
        : lexer_(lexer), engine_(engine), options_(std::move(options)) {}
    LRPatchLexer(const Lexer&&, const LRDiagEngine&, LRRecoveryOptions = {}) = delete;
    LRPatchLexer(const Lexer&, const LRDiagEngine&&, LRRecoveryOptions = {}) = delete;

    // Unpatched warnings and informs skip the invalid byte without editing
    // source. A later patch may restart the scan and diagnose that byte again.
    LRLexRecoveryResult lex(std::string_view source, std::string_view provenance = {}) const {
        LRLexRecoveryResult result;
        detail::LRRecoveryRun(result, source, provenance, engine_, options_, nullptr).lex(lexer_);
        return result;
    }

private:
    const Lexer& lexer_;
    const LRDiagEngine& engine_;
    LRRecoveryOptions options_;
};

class LRPatchParser {
public:
    LRPatchParser(const Lexer& lexer, const LRDiagEngine& engine, LRRecoveryOptions options = {})
        : lexer_(lexer), engine_(engine), options_(std::move(options)) {}
    LRPatchParser(const Lexer&&, const LRDiagEngine&, LRRecoveryOptions = {}) = delete;
    LRPatchParser(const Lexer&, const LRDiagEngine&&, LRRecoveryOptions = {}) = delete;

    // The callback must parse all tokens from scratch on each attempt and
    // report errors using current token indices (size() for EOF). The callback
    // must not retain references to the token vector. Lexical byte edits rebuild
    // all tokens; syntactic token edits leave source unchanged. Synthetic tokens
    // have zero-width spans at the start of the edited range (the next token or
    // EOF for insertion). Unpatched syntax errors are fatal regardless of
    // severity; on fatal, tokens describe the last attempt or scanned prefix.
    // Non-skipped lexer IDs are passed through unchanged and must be input
    // terminals of grammar (not EOF). Misconfigured IDs throw
    // std::invalid_argument before the first parse attempt.
    LRParseRecoveryResult parse(std::string_view source, const Grammar& grammar,
                                const LRParseCallback& parser, std::string_view provenance = {}) const {
        LRParseRecoveryResult result;
        detail::LRRecoveryRun run(result, source, provenance, engine_, options_, &grammar);
        if (!run.lex(lexer_)) return result;
        validateInput(grammar, result.tokens);
        for (;;) {
            auto attempt = parser(result.tokens);
            if (auto* tree = std::get_if<ParseNode>(&attempt)) {
                result.tree = std::move(*tree);
                return result;
            }
            if (run.syntaxError(std::get<LLSyntaxError>(attempt)) == detail::LRRecoveryRun::Step::fatal)
                return result;
        }
    }

    LRParseRecoveryResult parse(std::string_view source, const LLTableDriven& parser,
                                std::string_view provenance = {}) const {
        return parse(source, parser.table().grammar(), [&](const std::vector<Symbol>& tokens) {
            return parser.parseWithDiagnostics(tokens);
        }, provenance);
    }

private:
    const Lexer& lexer_;
    const LRDiagEngine& engine_;
    LRRecoveryOptions options_;
};

} // namespace firestone::lr
