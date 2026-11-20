#pragma once

#include "state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace domios {

constexpr std::uint32_t kDitMagic = 0x54494D44u;
constexpr std::uint32_t kDitPageSize = 4096u;
constexpr std::uint64_t kDitDefaultBase = 0x1000u;

enum class DitFormat : std::uint8_t {
    unknown = 0,
    xrgb8888 = 1,
    bgrx8888 = 2,
    rgb565 = 3,
    vga_text = 4
};

#pragma pack(push, 1)
struct DitHeader {
    std::uint32_t magic;
    std::uint16_t version_major;
    std::uint16_t version_minor;
    std::uint32_t table_size;
    std::uint32_t checksum;
    std::uint32_t sequence;
    std::uint8_t phase;
    std::uint8_t backend;
    std::uint16_t flags;
    std::uint64_t reserved0;
};

struct DitFramebuffer {
    std::uint64_t base;
    std::uint32_t pitch;
    std::uint32_t width;
    std::uint32_t height;
    std::uint8_t bpp;
    std::uint8_t format;
    std::uint16_t reserved1;
    std::uint64_t reserved2;
};

struct DitBootProgress {
    std::uint8_t percent;
    std::uint8_t stage;
    std::uint16_t reserved3;
    std::uint32_t msg_offset;
    std::uint32_t msg_len;
    std::uint32_t reserved4;
};

struct DitRegions {
    std::uint32_t strpool_offset;
    std::uint32_t strpool_len;
    std::uint32_t diag_offset;
    std::uint32_t diag_len;
};

struct DitPage {
    DitHeader header;
    DitFramebuffer framebuffer;
    DitBootProgress progress;
    DitRegions regions;
    std::uint8_t reserved[0x100u - 0x60u];
    std::uint8_t strpool[0x700u];
    std::uint8_t diagnostics[0x800u];
};
#pragma pack(pop)

static_assert(sizeof(DitHeader) == 0x20, "DitHeader size");
static_assert(sizeof(DitFramebuffer) == 0x20, "DitFramebuffer size");
static_assert(sizeof(DitBootProgress) == 0x10, "DitBootProgress size");
static_assert(sizeof(DitRegions) == 0x10, "DitRegions size");
static_assert(sizeof(DitPage) == kDitPageSize, "DitPage size");
static_assert(offsetof(DitPage, header) == 0x0, "header offset");
static_assert(offsetof(DitPage, framebuffer) == 0x20, "framebuffer offset");
static_assert(offsetof(DitPage, progress) == 0x40, "progress offset");
static_assert(offsetof(DitPage, regions) == 0x50, "regions offset");
static_assert(offsetof(DitPage, reserved) == 0x60, "reserved offset");
static_assert(offsetof(DitPage, strpool) == 0x100, "strpool offset");
static_assert(offsetof(DitPage, diagnostics) == 0x800, "diagnostics offset");

struct DitImage {
    std::array<std::uint8_t, kDitPageSize> bytes{};
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;
DitPage make_default_dit();
DitImage serialize_dit(const DitPage& page) noexcept;
bool validate_dit_base(std::uint64_t base, std::uint64_t mapped_begin, std::uint64_t mapped_end) noexcept;

} // namespace domios
