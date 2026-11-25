#include "MemoryLocation.hpp"

#include <algorithm>
#include <limits>

namespace wetstone::alias {

std::optional<std::int64_t> checked_offset_add(std::int64_t a, std::int64_t b) {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b)) return std::nullopt;
    return a + b;
}

std::optional<ByteRange> checked_range(std::int64_t begin, std::uint64_t size) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    if (begin >= 0) {
        if (size > static_cast<std::uint64_t>(max - begin)) return std::nullopt;
        return ByteRange{begin, begin + static_cast<std::int64_t>(size)};
    }

    // Compute |begin| without negating INT64_MIN. Split at zero so sizes
    // larger than INT64_MAX still work when the resulting endpoint fits.
    const auto magnitude = static_cast<std::uint64_t>(-(begin + 1)) + 1;
    if (size < magnitude) return ByteRange{begin, begin + static_cast<std::int64_t>(size)};
    const auto end = size - magnitude;
    if (end > static_cast<std::uint64_t>(max)) return std::nullopt;
    return ByteRange{begin, static_cast<std::int64_t>(end)};
}

std::optional<ByteRange> byte_range(const MemoryLocation& location) {
    if (!location.offset || !location.size) return std::nullopt;
    return checked_range(*location.offset, *location.size);
}

std::optional<MemoryAccess> memory_access(const ir::Instruction& instruction, ir::BlockId block,
                                          std::size_t index, std::optional<std::uint64_t> size) {
    const bool load = instruction.opcode == ir::Opcode::Load;
    if (!load && instruction.opcode != ir::Opcode::Store) return std::nullopt;

    MemoryAccess access{block, index, AccessKind::Unknown, {}};
    if (instruction.arguments.size() != (load ? 1u : 2u) ||
        instruction.result.has_value() != load ||
        (instruction.result && *instruction.result == ir::invalid_value) ||
        !instruction.targets.empty() || !instruction.phi_predecessors.empty() ||
        std::any_of(instruction.arguments.begin(), instruction.arguments.end(), [](const auto& operand) {
            return !operand.is_constant && operand.value == ir::invalid_value;
        })) return access;

    access.kind = load ? AccessKind::Read : AccessKind::Write;
    const auto& address = instruction.arguments.front();
    access.location = {address.is_constant ? ir::invalid_value : address.value, 0, size};
    return access;
}

} // namespace wetstone::alias
