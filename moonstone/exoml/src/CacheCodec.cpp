#include "ExtractCache.hpp"
#include <limits>
#include <type_traits>

namespace moonstone::exoml::cdecl::detail {
namespace {
// One field traversal is shared by both directions; no pointer representations,
// padding, host-endian data or compiler enum representations cross this boundary.
template <class A, class T> void fields(A &a, T &t) {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, TranslationUnit>)
        a(t.version, t.header, t.revision, t.fingerprint, t.target, t.arguments, t.inputs, t.types,
          t.declarations, t.diagnostics, t.complete, t.compilation, t.preprocessing);
    else if constexpr (std::is_same_v<U, CompilationContext>)
        a(t.database, t.directory, t.file, t.driver, t.original_arguments, t.ignored_arguments);
    else if constexpr (std::is_same_v<U, Preprocessing>)
        a(t.definitions, t.expansions, t.final_macros, t.includes, t.skipped_ranges,
          t.final_snapshot_complete, t.coverage);
    else if constexpr (std::is_same_v<U, MacroDefinition>)
        a(t.id, t.name, t.tokens, t.source, t.function_like, t.builtin);
    else if constexpr (std::is_same_v<U, MacroExpansion>)
        a(t.name, t.definition_id, t.unknown_reason, t.source);
    else if constexpr (std::is_same_v<U, MacroState>)
        a(t.name, t.definition_id, t.unknown_reason, t.defined);
    else if constexpr (std::is_same_v<U, IncludeEdge>)
        a(t.included, t.source);
    else if constexpr (std::is_same_v<U, Target>)
        a(t.compiler, t.triple, t.pointer_bits);
    else if constexpr (std::is_same_v<U, InputFile>)
        a(t.path, t.sha256);
    else if constexpr (std::is_same_v<U, Type>)
        a(t.id, t.canonical_id, t.spelling, t.canonical_spelling, t.declaration_id, t.kind,
          t.qualifiers, t.size, t.alignment, t.array_count, t.is_signed, t.element, t.result,
          t.arguments, t.fields, t.enumerators, t.variadic, t.prototype, t.is_union,
          t.calling_convention, t.attributes, t.unsupported_reason);
    else if constexpr (std::is_same_v<U, Qualifiers>)
        a(t.is_const, t.is_volatile, t.is_restrict);
    else if constexpr (std::is_same_v<U, Field>)
        a(t.name, t.type, t.source, t.offset_bits, t.bit_width);
    else if constexpr (std::is_same_v<U, Enumerator>)
        a(t.name, t.value);
    else if constexpr (std::is_same_v<U, Declaration>)
        a(t.id, t.name, t.type, t.kind, t.source, t.parameters, t.definition, t.is_inline,
          t.function_like_macro, t.linkage, t.tokens, t.unsupported_reason);
    else if constexpr (std::is_same_v<U, Parameter>)
        a(t.name, t.type, t.source, t.tokens);
    else if constexpr (std::is_same_v<U, SourceSpan>)
        a(t.file, t.line, t.column, t.end_line, t.end_column);
    else if constexpr (std::is_same_v<U, Diagnostic>)
        a(t.id, t.message, t.severity, t.primary, t.related, t.notes, t.fixes);
    else if constexpr (std::is_same_v<U, Fix>)
        a(t.span, t.replacement, t.description, t.validated);
    else
        static_assert(!sizeof(T), "missing cache field traversal");
}
struct Writer {
    std::string bytes = "EXOML-UNIT-2\n";
    template <class... T> void operator()(const T &...t) { (one(t), ...); }
    void one(bool b) { bytes += b ? '\1' : '\0'; }
    template <class T>
        requires std::is_unsigned_v<T>
    void one(T n) {
        auto v = static_cast<std::uint64_t>(n);
        for (unsigned i = 0; i < 8; ++i) {
            bytes += static_cast<char>(v & 255);
            v >>= 8;
        }
    }
    void one(const std::string &s) {
        one(s.size());
        bytes += s;
    }
    template <class T>
        requires std::is_enum_v<T>
    void one(T e) {
        one(static_cast<unsigned>(e));
    }
    template <class T> void one(const std::optional<T> &v) {
        one(bool(v));
        if (v)
            one(*v);
    }
    template <class T> void one(const std::vector<T> &v) {
        one(v.size());
        for (const auto &item : v)
            one(item);
    }
    template <class K, class V> void one(const std::map<K, V> &v) {
        one(v.size());
        for (const auto &[key, value] : v) {
            one(key);
            one(value);
        }
    }
    template <class T>
        requires(!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
    void one(const T &t) {
        fields(*this, t);
    }
};
struct Reader {
    std::string_view bytes;
    const ExtractLimits &limits;
    std::size_t entries = 0;
    [[noreturn]] static void fail() { throw std::invalid_argument("invalid/oversized cache unit"); }
    void charge() {
        if (entries++ >= limits.entries)
            fail();
    }
    std::string_view take(std::size_t n) {
        if (n > bytes.size())
            fail();
        auto out = bytes.substr(0, n);
        bytes.remove_prefix(n);
        return out;
    }
    template <class... T> void operator()(T &...t) { (one(t), ...); }
    void one(bool &b) {
        auto v = static_cast<unsigned char>(take(1)[0]);
        if (v > 1)
            fail();
        b = v != 0;
    }
    template <class T>
        requires std::is_unsigned_v<T>
    void one(T &v) {
        std::uint64_t n = 0;
        auto s = take(8);
        for (unsigned i = 0; i < 8; ++i)
            n |= std::uint64_t(static_cast<unsigned char>(s[i])) << (8 * i);
        if (n > std::numeric_limits<T>::max())
            fail();
        v = static_cast<T>(n);
    }
    void one(std::string &s) {
        std::size_t n = 0;
        one(n);
        s = take(n);
    }
    template <class T>
        requires std::is_enum_v<T>
    void one(T &e) {
        unsigned n = 0;
        one(n);
        unsigned max = 0;
        if constexpr (std::is_same_v<T, TypeKind>)
            max = unsigned(TypeKind::Elaborated);
        else if constexpr (std::is_same_v<T, DeclKind>)
            max = unsigned(DeclKind::Macro);
        else if constexpr (std::is_same_v<T, Severity>)
            max = unsigned(Severity::Fatal);
        else
            static_assert(!sizeof(T), "missing enum range");
        if (n > max)
            fail();
        e = static_cast<T>(n);
    }
    template <class T> void one(std::optional<T> &v) {
        bool present = false;
        one(present);
        if (present) {
            T t{};
            one(t);
            v = std::move(t);
        }
    }
    template <class T> void one(std::vector<T> &v) {
        std::size_t count = 0;
        one(count);
        if (count > limits.entries - std::min(entries, limits.entries))
            fail();
        for (std::size_t i = 0; i < count; ++i) {
            charge();
            T t{};
            one(t);
            v.push_back(std::move(t));
        }
    }
    template <class K, class V> void one(std::map<K, V> &v) {
        std::size_t count = 0;
        one(count);
        if (count > limits.entries - std::min(entries, limits.entries))
            fail();
        for (std::size_t i = 0; i < count; ++i) {
            charge();
            K key{};
            V value{};
            one(key);
            one(value);
            if (!v.emplace(std::move(key), std::move(value)).second)
                fail();
        }
    }
    template <class T>
        requires(!std::is_arithmetic_v<T> && !std::is_enum_v<T>)
    void one(T &t) {
        fields(*this, t);
    }
};
} // namespace
std::string encode_cache_unit(const TranslationUnit &unit) {
    Writer writer;
    writer(unit);
    return std::move(writer.bytes);
}
TranslationUnit decode_cache_unit(std::string_view bytes, const ExtractLimits &limits) {
    if (bytes.size() > limits.cache_bytes)
        Reader::fail();
    Reader reader{bytes, limits};
    if (reader.take(13) != "EXOML-UNIT-2\n")
        Reader::fail();
    TranslationUnit unit;
    reader(unit);
    if (!reader.bytes.empty() || unit.version != schema_version || !unit.complete ||
        !unit.preprocessing.final_snapshot_complete || unit.types.size() > limits.types ||
        unit.declarations.size() > limits.declarations)
        Reader::fail();
    auto reference = [&](const std::string &id, bool optional = false) {
        if ((!optional || !id.empty()) && !unit.types.contains(id))
            Reader::fail();
    };
    for (const auto &[id, type] : unit.types) {
        if (id.empty() || id != type.id)
            Reader::fail();
        reference(type.canonical_id);
        reference(type.element, true);
        reference(type.result, true);
        for (const auto &arg : type.arguments)
            reference(arg);
        for (const auto &field : type.fields)
            reference(field.type);
    }
    for (const auto &decl : unit.declarations) {
        reference(decl.type, decl.kind == DeclKind::Macro || decl.kind == DeclKind::Unsupported);
        for (const auto &param : decl.parameters)
            reference(param.type);
    }
    for (const auto &d : unit.diagnostics)
        if (d.severity == Severity::Error || d.severity == Severity::Fatal)
            Reader::fail();
    std::set<std::string> macros, names;
    for (const auto &d : unit.preprocessing.definitions)
        if (d.id.empty() || !macros.insert(d.id).second)
            Reader::fail();
    for (const auto &e : unit.preprocessing.expansions)
        if (!e.definition_id.empty() && !macros.contains(e.definition_id))
            Reader::fail();
    for (const auto &state : unit.preprocessing.final_macros)
        if (!state.defined || !names.insert(state.name).second ||
            (!state.definition_id.empty() && !macros.contains(state.definition_id)))
            Reader::fail();
    auto fingerprint = std::move(unit.fingerprint);
    unit.fingerprint.clear();
    if (sha256(describe_translation_unit(unit)) != fingerprint)
        Reader::fail();
    unit.fingerprint = std::move(fingerprint);
    return unit;
}
} // namespace moonstone::exoml::cdecl::detail
