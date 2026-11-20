#include "dit.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace domios {
namespace {

std::uint32_t crc32_update(std::uint32_t crc, std::uint8_t byte) noexcept {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 1u) ? (crc >> 1u) ^ 0xEDB88320u : (crc >> 1u);
    }
    return crc;
}

} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = crc32_update(crc, data[i]);
    }
    return ~crc;
}

DitPage make_default_dit() {
    DitPage page{};
    page.header.magic = kDitMagic;
    page.header.version_major = 1;
    page.header.version_minor = 0;
    page.header.table_size = kDitPageSize;
    page.header.checksum = 0;
    page.header.sequence = 0;
    page.header.phase = static_cast<std::uint8_t>(Phase::bootstrap);
    page.header.backend = static_cast<std::uint8_t>(BackendKind::none);
    page.header.flags = 0;
    page.header.reserved0 = 0;

    page.framebuffer.base = 0;
    page.framebuffer.pitch = 0;
    page.framebuffer.width = 0;
    page.framebuffer.height = 0;
    page.framebuffer.bpp = 0;
    page.framebuffer.format = static_cast<std::uint8_t>(DitFormat::unknown);
    page.framebuffer.reserved1 = 0;
    page.framebuffer.reserved2 = 0;

    page.progress.percent = 0;
    page.progress.stage = static_cast<std::uint8_t>(Phase::bootstrap);
    page.progress.reserved3 = 0;
    page.progress.msg_offset = 0;
    page.progress.msg_len = 0;
    page.progress.reserved4 = 0;

    page.regions.strpool_offset = 0x100u;
    page.regions.strpool_len = 0x700u;
    page.regions.diag_offset = 0x800u;
    page.regions.diag_len = 0x800u;

    std::fill(std::begin(page.reserved), std::end(page.reserved), 0);
    std::fill(std::begin(page.strpool), std::end(page.strpool), 0);
    std::fill(std::begin(page.diagnostics), std::end(page.diagnostics), 0);

    auto image = serialize_dit(page);
    std::memcpy(&page, image.bytes.data(), image.bytes.size());
    return page;
}

DitImage serialize_dit(const DitPage& page) noexcept {
    DitImage image{};
    std::memcpy(image.bytes.data(), &page, sizeof(DitPage));
    auto* bytes = image.bytes.data();
    const auto checksum_offset = offsetof(DitPage, header) + offsetof(DitHeader, checksum);
    bytes[checksum_offset + 0] = 0;
    bytes[checksum_offset + 1] = 0;
    bytes[checksum_offset + 2] = 0;
    bytes[checksum_offset + 3] = 0;
    const std::uint32_t checksum = crc32(bytes, image.bytes.size());
    std::memcpy(bytes + checksum_offset, &checksum, sizeof(checksum));
    return image;
}

bool validate_dit_base(std::uint64_t base, std::uint64_t mapped_begin, std::uint64_t mapped_end) noexcept {
    return base % 0x1000u == 0u && base >= mapped_begin && base + kDitPageSize <= mapped_end;
}

} // namespace domios

