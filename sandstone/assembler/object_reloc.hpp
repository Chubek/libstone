// Instruction encoders may leave "symbol +/- constant" in the reference;
// data directives normally supply the symbol and addend separately.
#pragma once
#include "intern.h"
#include <stdexcept>
#include <string>

struct SandObjectRef {
    std::string name;
    uint64_t addend;
};

inline SandObjectRef sand_object_ref(sandstone_asm_t *ctx, const sand_reloc_intern_t &r) {
    SandObjectRef ref{r.symbol, uint64_t(r.addend)};
    if (sand_is_name(ref.name.c_str())) return ref;
    auto pos = ref.name.find_first_of("+-");
    int ok = 0;
    if (pos != std::string::npos) {
        auto delta = uint64_t(sand_eval_expr(ctx, ref.name.c_str() + pos + 1, &ok));
        ref.addend += ref.name[pos] == '-' ? -delta : delta;
        ref.name.resize(pos);
        ref.name.erase(ref.name.find_last_not_of(" \t") + 1);
    }
    if (!ok || !sand_is_name(ref.name.c_str()))
        throw std::runtime_error("unsupported object symbol expression: " + std::string(r.symbol));
    return ref;
}
