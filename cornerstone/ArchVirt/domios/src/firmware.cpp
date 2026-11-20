#include "firmware.hpp"
#include "dit.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace domios {
namespace {
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
std::uint64_t get(std::span<const std::uint8_t> bytes, std::size_t at, unsigned size) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(bytes[at + i]) << (8 * i);
    return value;
}
}
std::vector<std::uint8_t> pack_firmware(const std::vector<FirmwarePart>& parts) {
    if (parts.size() != 5) throw std::invalid_argument("DomIOS requires five firmware sections");
    std::vector<std::uint8_t> image{'D','O','M','I','O','S','2',0};
    put(image, 2, 4); put(image, parts.size(), 4);
    std::uint64_t offset = 16 + 24 * parts.size();
    for (const auto& part : parts) {
        put(image, static_cast<unsigned>(part.kind), 4);
        put(image, crc32(part.bytes.data(), part.bytes.size()), 4);
        put(image, offset, 8); put(image, part.bytes.size(), 8);
        offset += part.bytes.size();
    }
    for (const auto& part : parts) image.insert(image.end(), part.bytes.begin(), part.bytes.end());
    std::vector<FirmwarePart> checked; std::string error;
    if (!unpack_firmware(image, checked, error)) throw std::invalid_argument(error);
    return image;
}
bool unpack_firmware(std::span<const std::uint8_t> image, std::vector<FirmwarePart>& parts, std::string& error) {
    auto fail = [&](const char* message) { error = message; return false; };
    if (image.size() < 136 || std::memcmp(image.data(), "DOMIOS2\0", 8) ||
        get(image, 8, 4) != 2 || get(image, 12, 4) != 5) return fail("Invalid DomIOS firmware header");
    std::vector<FirmwarePart> result;
    unsigned seen = 0;
    std::uint64_t previous_end = 136;
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned at = 16 + i * 24;
        auto kind = get(image, at, 4), crc = get(image, at + 4, 4), offset = get(image, at + 8, 8), size = get(image, at + 16, 8);
        if (kind < 1 || kind > 5 || (seen & (1u << kind)) || offset != previous_end ||
            offset > image.size() || size > image.size() - offset || size > 1024 * 1024)
            return fail("Invalid DomIOS firmware section bounds or type");
        seen |= 1u << kind;
        const auto data = image.subspan(offset, size);
        if (crc32(data.data(), data.size()) != crc) return fail("DomIOS firmware checksum mismatch");
        if (kind == 1 && (size != kDitPageSize || get(data, 0, 4) != kDitMagic))
            return fail("Invalid DomIOS display information table");
        result.push_back({static_cast<FirmwareSection>(kind), {data.begin(), data.end()}});
        previous_end = offset + size;
    }
    if (previous_end != image.size()) return fail("Trailing bytes in DomIOS firmware");
    parts = std::move(result);
    return true;
}
}
