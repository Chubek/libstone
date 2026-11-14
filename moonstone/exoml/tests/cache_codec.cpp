#include "ExtractCache.hpp"
#include <cassert>
#include <limits>

using namespace moonstone::exoml::cdecl;
using namespace moonstone::exoml::cdecl::detail;
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    assert(rejected);
}
int main() {
    ExtractOptions options;
    options.header = EXOML_DISCOVERY_FIXTURE;
    options.resource_directory = EXOML_CLANG_RESOURCES;
    auto unit = extract_c11(options);
    assert(unit.complete);
    unit.compilation = CompilationContext{
        "database", "directory", "file", "driver", {"driver", "-c", "file"}, {"-c", "file"}};
    moonstone::Diagnostic note;
    note.id = "CACHE-ROUNDTRIP";
    note.message = std::string("message\0with bytes", 18);
    note.severity = moonstone::Severity::Warning;
    note.primary = {unit.header, 1, 2, 3, 4};
    note.related = {{unit.header, 5, 6, 7, 8}};
    note.notes = {"note", "\"\\\n"};
    note.fixes.push_back({note.primary, "replacement", "description", false});
    unit.diagnostics.push_back(note);
    unit.types.begin()->second.size = std::numeric_limits<std::uint64_t>::max();
    unit.fingerprint.clear();
    unit.fingerprint = sha256(describe_translation_unit(unit));
    auto encoded = encode_cache_unit(unit);
    auto decoded = decode_cache_unit(encoded, options.limits);
    assert(encode_cache_unit(decoded) == encoded);
    assert(describe_translation_unit(decoded) == describe_translation_unit(unit));
    assert(decoded.diagnostics.back().notes == note.notes);
    assert(decoded.diagnostics.back().fixes[0].replacement == "replacement");
    auto key = normalization_cache_key(unit, options.limits, "ast-one", "build-one");
    assert(key != normalization_cache_key(unit, options.limits, "ast-one", "build-two"));
    assert(key != normalization_cache_key(unit, options.limits, "ast-two", "build-one"));
    auto changed_limits = options.limits;
    --changed_limits.depth;
    assert(key != normalization_cache_key(unit, changed_limits, "ast-one", "build-one"));
    for (auto n : {std::size_t(0), std::size_t(12), std::size_t(13), encoded.size() - 1})
        rejects([&] { decode_cache_unit(std::string_view(encoded).substr(0, n), options.limits); });
    rejects([&] { decode_cache_unit(encoded + 'x', options.limits); });
    auto corrupt = encoded;
    corrupt[0] = '!';
    rejects([&] { decode_cache_unit(corrupt, options.limits); });
    corrupt = encoded;
    corrupt[corrupt.size() - unit.preprocessing.coverage.size() - 8 - 1] = 2;
    // Boolean encodings are exactly 0 or 1.
    rejects([&] { decode_cache_unit(corrupt, options.limits); });
    corrupt = encoded;
    for (std::size_t i = 21; i < 29; ++i)
        corrupt[i] = char(255); // Header string length.
    rejects([&] { decode_cache_unit(corrupt, options.limits); });
    for (auto mutate : {0, 1, 2, 3, 4}) {
        auto invalid = unit;
        if (mutate == 0)
            invalid.version = 999;
        if (mutate == 1)
            invalid.types.begin()->second.kind = static_cast<TypeKind>(999);
        if (mutate == 2)
            invalid.types.begin()->second.canonical_id = "missing";
        if (mutate == 3)
            invalid.fingerprint = "wrong";
        if (mutate == 4)
            invalid.diagnostics.back().severity = static_cast<moonstone::Severity>(999);
        rejects([&] { decode_cache_unit(encode_cache_unit(invalid), options.limits); });
    }
    auto limits = options.limits;
    limits.cache_bytes = 1;
    rejects([&] { decode_cache_unit(encoded, limits); });
    limits = options.limits;
    limits.types = 0;
    rejects([&] { decode_cache_unit(encoded, limits); });
    limits = options.limits;
    limits.declarations = 0;
    rejects([&] { decode_cache_unit(encoded, limits); });
    limits = options.limits;
    limits.entries = 1;
    rejects([&] { decode_cache_unit(encoded, limits); });
}
