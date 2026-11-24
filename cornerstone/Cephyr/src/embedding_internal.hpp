#ifndef CEPHYR_EMBEDDING_INTERNAL_HPP
#define CEPHYR_EMBEDDING_INTERNAL_HPP

#include <cephyr/Cephyr.h>

#include <map>
#include <memory>
#include <span>
#include <string>
#include <sys/types.h>

struct cephyr_result {
    std::string output;
    std::string diagnostics;
    bool loadable_output = false;
    // Embedded host addresses are valid only in the process that linked them.
    pid_t host_binding_process = 0;
};

namespace cephyr::detail {
inline constexpr std::size_t max_source_size = 16u * 1024u * 1024u;
struct TranslationUnit {
    std::string name, source;
};
struct HostSymbol {
    const void *address;
    cephyr_host_symbol_kind kind;
};
using HostSymbols = std::map<std::string, HostSymbol>;
int compile_units(const cephyr_request *request,
                  std::span<const std::shared_ptr<const TranslationUnit>> units,
                  const HostSymbols *symbols, cephyr_result **result,
                  cephyr_ast_format ast_format = {}, bool emit_interface = false);
}

#endif
