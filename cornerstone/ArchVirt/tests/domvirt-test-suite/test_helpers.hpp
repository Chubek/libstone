#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace domvirt::test_support {

inline std::filesystem::path temp_dir() {
    return std::filesystem::temp_directory_path() / "domvirt-test-suite";
}

inline std::filesystem::path make_temp_path(std::string_view stem, std::string_view suffix) {
    static std::atomic<uint64_t> counter{0};
    std::filesystem::create_directories(temp_dir());
    return temp_dir() /
        (std::string(stem) + "-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
         std::string(suffix));
}

inline std::filesystem::path write_temp_bytes(std::string_view stem, std::span<const std::uint8_t> data) {
    auto path = make_temp_path(stem, ".bin");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

inline std::filesystem::path write_temp_text(std::string_view stem, std::string_view text) {
    auto path = make_temp_path(stem, ".txt");
    std::ofstream out(path);
    out << text;
    return path;
}

inline std::vector<std::uint8_t> make_buffer(std::size_t size, std::uint8_t fill = 0) {
    return std::vector<std::uint8_t>(size, fill);
}

inline void write_u16(std::vector<std::uint8_t>& mem, std::size_t off, std::uint16_t value) {
    mem[off + 0] = static_cast<std::uint8_t>(value & 0xff);
    mem[off + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

inline void write_u32(std::vector<std::uint8_t>& mem, std::size_t off, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        mem[off + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
    }
}

inline void write_u64(std::vector<std::uint8_t>& mem, std::size_t off, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        mem[off + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
    }
}

inline std::uint16_t read_u16(const std::vector<std::uint8_t>& mem, std::size_t off) {
    return static_cast<std::uint16_t>(mem[off]) |
           static_cast<std::uint16_t>(mem[off + 1]) << 8;
}

inline std::uint32_t read_u32(const std::vector<std::uint8_t>& mem, std::size_t off) {
    return static_cast<std::uint32_t>(mem[off]) |
           (static_cast<std::uint32_t>(mem[off + 1]) << 8) |
           (static_cast<std::uint32_t>(mem[off + 2]) << 16) |
           (static_cast<std::uint32_t>(mem[off + 3]) << 24);
}

inline std::uint64_t read_u64(const std::vector<std::uint8_t>& mem, std::size_t off) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(mem[off + i]) << (i * 8);
    }
    return value;
}

} // namespace domvirt::test_support
