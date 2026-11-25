#pragma once

#include "../utils/IR.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wetstone::alias {

// A byte range relative to one observation of an SSA pointer. None means
// unknown; zero size denotes an empty access. Locations describe valid,
// non-wrapping accesses within the pointer's storage object.
struct MemoryLocation {
    ir::ValueId pointer = ir::invalid_value;
    std::optional<std::int64_t> offset = 0;
    std::optional<std::uint64_t> size = std::nullopt;
};

struct ByteRange {
    std::int64_t begin;
    std::int64_t end; // exclusive

    [[nodiscard]] constexpr bool valid() const noexcept { return begin <= end; }

    // Positional containment, including empty ranges at either boundary.
    // Malformed ranges are never accepted as containment proofs.
    [[nodiscard]] constexpr bool contains(const ByteRange& other) const noexcept {
        return valid() && other.valid() && begin <= other.begin && other.end <= end;
    }
};

// Checked arithmetic for offsets in the signed relative-coordinate space.
[[nodiscard]] std::optional<std::int64_t> checked_offset_add(std::int64_t a, std::int64_t b);
[[nodiscard]] std::optional<ByteRange> checked_range(std::int64_t offset, std::uint64_t size);

// Relative endpoints only; this does not resolve or validate the base pointer.
[[nodiscard]] std::optional<ByteRange> byte_range(const MemoryLocation& location);

enum class AccessKind { Read, Write, ReadWrite, Unknown };

struct MemoryAccess {
    ir::BlockId block = ir::invalid_block;
    std::size_t instruction = 0;
    AccessKind kind = AccessKind::Unknown;
    MemoryLocation location;
};

// Recognize Load and Store only. Calls and other effects must be handled
// separately; absence here does not mean an instruction is memory-free.
// Bad Load/Store shapes produce an Unknown access, not a missing access.
// The caller may supply a trusted size; the current IR does not carry one.
[[nodiscard]] std::optional<MemoryAccess>
memory_access(const ir::Instruction& instruction, ir::BlockId block, std::size_t index,
              std::optional<std::uint64_t> size = {});

} // namespace wetstone::alias
