#pragma once
#include <clang-c/Index.h>
#include <exoml/Extract.hpp>
#include <memory>

namespace moonstone::exoml::cdecl::detail {
std::string sha256(std::string_view bytes);
// Internal wire format, not a native-object dump or a second declaration schema.
std::string encode_cache_unit(const TranslationUnit &unit);
TranslationUnit decode_cache_unit(std::string_view bytes, const ExtractLimits &limits);
std::string normalization_cache_key(const TranslationUnit &evidence, const ExtractLimits &limits,
                                    std::string_view ast_hash, std::string_view build_identity);
class NormalizationCache {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    NormalizationCache(const ExtractOptions &, CacheReport &, const TranslationUnit &evidence,
                       CXTranslationUnit);
    ~NormalizationCache();
    std::optional<TranslationUnit> load();
    void store(const TranslationUnit &unit);
};
} // namespace moonstone::exoml::cdecl::detail
