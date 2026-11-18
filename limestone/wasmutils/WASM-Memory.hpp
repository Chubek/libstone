#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace limestone::wasmutils {

// Fixed-size, zero-initialized wasm32 storage, independent of the interpreter.
// Accesses are little-endian and may be unaligned. The wide address parameter
// allows callers to pass an effective address without wrapping it to 32 bits.
class LinearMemory {
public:
    static constexpr std::uint64_t address_space_size = std::uint64_t{1} << 32;

    // Throws std::length_error if the size exceeds wasm32 or the host limit.
    explicit LinearMemory(std::uint64_t byte_count = 0) {
        if (byte_count > address_space_size || byte_count > bytes_.max_size())
            throw std::length_error("wasm32 linear memory size exceeds limit");
        bytes_.resize(static_cast<std::size_t>(byte_count));
    }

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    // A load or store crossing the bounds throws std::out_of_range. A failed
    // store leaves all bytes unchanged.
    [[nodiscard]] std::uint8_t load8(std::uint64_t address) const {
        return static_cast<std::uint8_t>(load(address, 1));
    }

    [[nodiscard]] std::uint16_t load16(std::uint64_t address) const {
        return static_cast<std::uint16_t>(load(address, 2));
    }

    [[nodiscard]] std::uint32_t load32(std::uint64_t address) const {
        return static_cast<std::uint32_t>(load(address, 4));
    }

    [[nodiscard]] std::uint64_t load64(std::uint64_t address) const {
        return load(address, 8);
    }

    void store8(std::uint64_t address, std::uint8_t bits) { store(address, 1, bits); }
    void store16(std::uint64_t address, std::uint16_t bits) { store(address, 2, bits); }
    void store32(std::uint64_t address, std::uint32_t bits) { store(address, 4, bits); }
    void store64(std::uint64_t address, std::uint64_t bits) { store(address, 8, bits); }

private:
    [[nodiscard]] std::size_t checked_start(std::uint64_t address, unsigned width) const {
        const auto size = static_cast<std::uint64_t>(bytes_.size());
        if (address > size || width > size - address)
            throw std::out_of_range("out-of-bounds wasm32 linear memory access");
        return static_cast<std::size_t>(address);
    }

    [[nodiscard]] std::uint64_t load(std::uint64_t address, unsigned width) const {
        const auto start = checked_start(address, width);
        std::uint64_t bits = 0;
        for (unsigned byte = 0; byte < width; ++byte)
            bits |= std::uint64_t{bytes_[start + byte]} << (byte * 8);
        return bits;
    }

    void store(std::uint64_t address, unsigned width, std::uint64_t bits) {
        const auto start = checked_start(address, width);
        for (unsigned byte = 0; byte < width; ++byte)
            bytes_[start + byte] = static_cast<std::uint8_t>(bits >> (byte * 8));
    }

    std::vector<std::uint8_t> bytes_;
};

} // namespace limestone::wasmutils
