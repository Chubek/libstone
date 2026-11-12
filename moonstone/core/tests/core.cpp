#include <cassert>
#include <core/CheckedArithmetic.hpp>
#include <core/Diagnostics.hpp>
#include <core/SExpr.hpp>
#include <core/SourceManager.hpp>
#include <limits>
#include <thread>

using namespace moonstone;

static void sources() {
    SourceManager manager;
    // a, two-byte e-acute, LF, four-byte emoji, CRLF, z
    const std::string text = "a\xc3\xa9\n\xf0\x9f\x98\x80\r\nz";
    auto id = manager.add("test.ex", text);
    assert(id && id.value() == 1);
    auto span = manager.span(id.value(), 1, 8);
    assert(span && span.value().file == "test.ex");
    assert(span.value().line == 1 && span.value().column == 2);
    assert(span.value().end_line == 2 && span.value().end_column == 5);
    auto eof = manager.span(id.value(), text.size(), text.size());
    assert(eof && eof.value().line == 3 && eof.value().column == 2);
    assert(!manager.span(id.value(), 2, 3)); // Inside e-acute.
    assert(!manager.span(id.value(), 4, 7)); // Inside emoji.
    assert(!manager.span(id.value(), 8, 7));
    assert(!manager.span(id.value(), 0, text.size() + 1));
    assert(!manager.span(invalid_source, 0, 0));
    assert(!manager.snapshot(99));
    for (const auto &bad : {std::string("\xc0\x80"), std::string("\xe0\x80\x80"),
                            std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                            std::string("\x80"), std::string("\xf0\x9f"), std::string("\xc2x")}) {
        auto invalid = manager.add("bad", bad);
        assert(!invalid && invalid.error().category == ErrorCategory::InvalidInput);
        assert(!invalid.error().contexts.empty());
    }
    assert(!manager.add(std::string("a\0b", 3), ""));
    assert(!manager.add("\xff", ""));
    auto next = manager.add("empty", "");
    assert(next && next.value() == 2); // Failed additions consume no IDs.
    assert(manager.span(next.value(), 0, 0).value().column == 1);
    auto trailing = manager.add("lf", "x\n");
    assert(manager.span(trailing.value(), 2, 2).value().line == 2);

    SourceManager limited({1, 3});
    assert(!limited.add("a", "xxx"));
    assert(limited.add("a", "xx"));
    assert(!limited.add("", ""));
    SourceManager none({0, 0});
    assert(!none.add("", ""));
    std::shared_ptr<const SourceSnapshot> snapshot;
    {
        SourceManager temporary;
        snapshot = temporary.snapshot(temporary.add("saved", "data").value()).value();
    }
    assert(snapshot->text == "data");
}

static void symbols_and_threads() {
    SymbolTable limited(1, 3);
    assert(!limited.intern(""));
    assert(!limited.intern("long"));
    assert(limited.intern("abc").value() == 1);
    assert(limited.intern("abc").value() == 1);
    assert(!limited.intern("def"));
    assert(!limited.lookup(0));
    assert(!limited.lookup(2));
    assert(limited.lookup(1).value() == "abc");

    SourceManager manager;
    SymbolTable symbols;
    const auto common = symbols.intern("common").value();
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&, worker] {
            for (int n = 0; n < 32; ++n) {
                assert(symbols.intern("common").value() == common);
                auto spelling = std::to_string(worker) + ":" + std::to_string(n);
                auto symbol = symbols.intern(spelling);
                assert(symbol && symbols.lookup(symbol.value()).value() == spelling);
                auto source = manager.add(spelling, "a\nb");
                assert(source && manager.span(source.value(), 0, 3).value().end_line == 2);
                assert(manager.snapshot(source.value()).value()->file == spelling);
            }
        });
    }
    for (auto &thread : threads)
        thread.join();
}

static void diagnostics_and_results() {
    Result<int> good(42);
    assert(good && good.value() == 42);
    Result<int> bad(Error{ErrorCategory::Unsupported, "unsupported", {{"test", std::nullopt}}});
    assert(!bad && bad.error().category == ErrorCategory::Unsupported);
    bool threw = false;
    try {
        (void)bad.value();
    } catch (const std::bad_variant_access &) {
        threw = true;
    }
    assert(threw);
    Result<void> success;
    assert(success);
    success.value();
    Result<void> failure(bad.error());
    assert(!failure);

    DiagnosticOptions options;
    options.max_count = 1;
    options.max_bytes = 4;
    options.minimum_severity = Severity::Warning;
    options.suppressed_ids.insert("hidden");
    DiagnosticBuffer buffer(options);
    assert(!buffer.emit({"n", "", Severity::Note, {}, {}, {}, {}}));
    assert(!buffer.emit({"hidden", "error", Severity::Error, {}, {}, {}, {}}));
    assert(buffer.has_errors() && buffer.suppressed() == 2 && !buffer.exhausted());
    assert(!buffer.emit({"e", "huge message", Severity::Error, {}, {}, {}, {}}));
    assert(buffer.exhausted() && buffer.dropped() == 1);
    assert(buffer.emit({"e", "abc", Severity::Error, {}, {}, {}, {}}));
    assert(buffer.retained_bytes() == 4);
    assert(!buffer.emit({"e", "", Severity::Error, {}, {}, {}, {}}));
    assert(buffer.diagnostics().size() == 1 && buffer.dropped() == 2);
    DiagnosticBuffer empty({0, 0, Severity::Note, {}});
    assert(!empty.emit({"", "", Severity::Fatal, {}, {}, {}, {}}));
    assert(empty.has_errors() && empty.exhausted());
    DiagnosticBuffer notes({10, 4, Severity::Note, {}});
    Diagnostic with_note{"e", "", Severity::Error, {}, {}, {"long"}, {}};
    assert(!notes.emit(with_note));
    with_note.fixes.clear();
    with_note.notes.clear();
    with_note.notes.resize(1025);
    assert(!notes.emit(with_note)); // Empty strings still consume bounded metadata.
    with_note.notes.clear();
    with_note.fixes.push_back({{}, "long", "", false});
    assert(!notes.emit(with_note));
    DiagnosticBuffer malformed;
    malformed.emit(
        {"bad-severity", "unknown severity", static_cast<Severity>(999), {}, {}, {}, {}});
    assert(malformed.has_errors());
}

static void edits_and_sexprs() {
    assert(apply_edits("abc", {{1, 2, "X"}, {3, 3, "!"}}) == "aXc!");
    assert(apply_edits("abc", {}) == "abc");
    assert(!apply_edits("abc", {{0, 2, ""}, {1, 3, ""}}));
    assert(!apply_edits("abc", {{2, 1, ""}}));
    assert(!apply_edits("abc", {{0, 4, ""}}));
    assert(!apply_edits("abc", {{1, 1, "X"}, {1, 1, "Y"}}));
    assert(json_escape(std::string("\0\n\r\t\"\\", 6)) == "\\u0000\\n\\r\\t\\\"\\\\");
    for (auto input : {"(", ")", "\"unterminated", "\"\\q\""}) {
        bool threw = false;
        try {
            (void)sexpr::Parser(input).one();
        } catch (const std::invalid_argument &) {
            threw = true;
        }
        assert(threw);
    }
    for (const auto &atom :
         {std::string("\r"), std::string("a\v b"), std::string("a\nb"), std::string("a\\b")}) {
        auto node = sexpr::Node::symbol(atom);
        assert(sexpr::Parser(sexpr::print(node)).one() == node);
    }
    bool exhausted = false;
    try {
        (void)sexpr::Parser("((x))", 10, 1).one();
    } catch (const std::length_error &) {
        exhausted = true;
    }
    assert(exhausted);
    exhausted = false;
    try {
        (void)sexpr::Parser("(x y)", 2).one();
    } catch (const std::length_error &) {
        exhausted = true;
    }
    assert(exhausted);
}

static void arithmetic_tests() {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    assert(!arithmetic::add(max, 1));
    assert(!arithmetic::add(min, -1));
    assert(arithmetic::add(min, max) == -1);
    assert(!arithmetic::sub(min, 1));
    assert(!arithmetic::sub(max, -1));
    assert(arithmetic::sub(min, min) == 0);
    assert(!arithmetic::neg(min));
    assert(arithmetic::neg(max) == -max);
    assert(!arithmetic::mul(min, -1));
    assert(!arithmetic::mul(-1, min));
    assert(!arithmetic::mul(max, 2));
    assert(!arithmetic::mul(min, 2));
    assert(!arithmetic::mul(max, -2));
    assert(arithmetic::mul(min, 0) == 0);
    assert(arithmetic::mul(min, 1) == min);
    assert(arithmetic::mul(min / 2, 2) == min);
    assert(arithmetic::mul(-2, max / 2) == -max + 1);
    for (std::int64_t a = -100; a <= 100; ++a) {
        for (std::int64_t b = -100; b <= 100; ++b) {
            assert(arithmetic::add(a, b) == a + b);
            assert(arithmetic::sub(a, b) == a - b);
            assert(arithmetic::mul(a, b) == a * b);
        }
    }
}

int main() {
    assert(api_version == 1);
    sources();
    symbols_and_threads();
    diagnostics_and_results();
    edits_and_sexprs();
    arithmetic_tests();
}
