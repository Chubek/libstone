# Live faculties: integration contract v1

W01 owns `LibDSL.hpp` and integration. W02–W12 each own only `staging/Wnn.inc`
and `staging/Wnn.test.cpp`, plus a worker-specific example if useful.
Fragments are merged **inside `namespace dsl` before the final namespace close**.
No `#include` in fragments; ask W01 to add required standard headers. Everything
with external linkage must be inline, templated, or otherwise ODR-safe. Adapt
existing faculties; use `dsl::SExpr`/`parse_sexpr` for textual construction, and
have textual and native paths use the same validating builder. Use
`#if LIBDSL_LIVE_POSIX` for OS APIs and provide portable alternatives.

| Owner | Namespace / public interface | Dependencies |
|---|---|---|
| W02 | `live::SourceSpan { std::size_t begin, end; }` (first two fields, half-open bytes); `live::Diagnostic`, PMR storage and typed registry | — |
| W03 | `live::SyntaxStatus { complete, incomplete, invalid }`; `live::Token` and analysis adapters | W02 |
| W04 | `terminal::terminfo`, plus `terminal::InputDecoder`, terminal primitives; standalone runtime | — |
| W05 | `terminal::StyledText`, `terminal::RenderMode { true_color, color256, ansi, grayscale, monochrome, plain, debug }`, `terminal::display_width(std::string_view)`, `terminal::sanitize_text(std::string_view)`. `StyledText::text(std::string_view)` appends, `render(RenderMode)` returns string. `GlobalEscapeCodes` mixin. | — |
| W06 | `cli::OptParser`; `OptParser` mixin in `dsl`; take `terminal::RenderMode` for help. Keep directives as consumers of your public API. | W05 |
| W07 | `repl::Editor`, `repl::Session`, `repl::Evaluation`/callbacks as needed; `REPL` mixin. Core takes callbacks (including continuation predicate); analysis/directives install without core knowing concrete types. Use `terminal` glyph APIs. | W03–05 |
| W08 | `repl::Analysis`, configuration and providers; design independent display/cursor/analysis results usable by W07 without hard coupling. | W02–05 |
| W09 | `repl::Directives`, `repl::scripts`; public runner invoked only for explicitly trusted configuration; `LIBDSL_LIVE_POSIX` guard. Relay concrete OptParser API details via W01. | W06 |
| W10 | `synth::Specification`, `synth::MetaLanguage`, `LanguageSynthesizer` mixin; syntax and macro layer using `dsl::SExpr` as interchange. | W02 |
| W11 | `synth::AttributeEngine`, scoping and metaproc engine; use independent state templates and stable node keys, adapter accepting specification if suitable. | W02, W10 (optional) |
| W12 | `synth::Executable<State>` semantics over expressions/statements, reference languages via examples or tests; integrate via adapters, no forward dependency in earlier fragments. | W10–11 (optional), W07 |

Canonical layers must validate on every public execution/render path if their IR
is publicly mutable. Text from users must not become terminal controls. POSIX
process execution must not invoke a shell implicitly or grant a textual DSL an
implicit executable capability. Pure analysis must not invoke evaluation
callbacks. Budgets and byte offsets must be checked before recursion, indexing,
arithmetic, or narrowing. No silent success for unsupported capabilities.

Contract v2 (additive): the broker supplies a `parse_sexpr(text, SExprLimits{})`
overload for new untrusted input paths. The original single-argument function
retains its existing bounds and behavior. Terminal glyphs carry half-open UTF-8
byte offsets and display widths; REPL positions and completion replacements use
byte offsets, not columns. `LIBDSL_LIVE_POSIX=0` selects portable operation; set
it consistently in all translation units.

Terminal input adapters use `terminal::InputDecoder::feed` and `flush` and
`terminal::RawMode` for lexical events and terminal lifetime respectively.
Session analyzers and evaluators are separate injected callbacks. External script
execution is an explicitly trusted, opt-in adapter; loading a language or
directive schema must not run a program.

Contract v3 (integration): the bounded entry point ships as
`parse_bounded_sexpr(text, limits)` so existing code taking the address of
`parse_sexpr` remains source-compatible. This rename is mechanical at merge.

Milestones: v1 contract and worker ownership → isolated implementation and
tests → dependency-ordered integration into the single header → standalone,
portable, CTest/ODR and sanitizer verification → report remaining gaps.
