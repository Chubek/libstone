#include "WASM-Core.hpp"

#include <algorithm>
#include <iterator>

namespace limestone::wasmutils::core {
namespace {

constexpr OpcodeInfo inventory[] = {
#include "WASM-Core-Opcodes.inc"
};

static_assert(std::size(inventory) == 437);
static_assert([] {
    for (std::size_t i = 1; i < std::size(inventory); ++i)
        if (inventory[i - 1].opcode >= inventory[i].opcode)
            return false;
    return true;
}());

} // namespace

std::span<const OpcodeInfo> opcodes() noexcept {
    return inventory;
}

const OpcodeInfo* lookup(Opcode opcode) noexcept {
    const auto it = std::lower_bound(std::begin(inventory), std::end(inventory), opcode,
        [](const OpcodeInfo& info, Opcode target) { return info.opcode < target; });
    return it != std::end(inventory) && it->opcode == opcode ? it : nullptr;
}

const OpcodeInfo* lookup(std::string_view mnemonic, bool typed_select) noexcept {
    for (const auto& info : inventory)
        if (info.mnemonic == mnemonic &&
            (mnemonic != "select" || typed_select == (info.format == Format::types)))
            return &info;
    return nullptr;
}

} // namespace limestone::wasmutils::core
