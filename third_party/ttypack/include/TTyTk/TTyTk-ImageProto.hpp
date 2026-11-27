#pragma once

#include <TTyTk/TTyTk-Core.hpp>
#include <TTyTk/TTyTk-ColorTools.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ttytk::image {

/** Row-major RGB image shared by terminal graphics encoders and viewers. */
struct Image {
    std::size_t width{};
    std::size_t height{};
    std::vector<color::Rgb> pixels{};
};

namespace detail {

[[nodiscard]] inline auto dimensions_match(const Span<const color::Rgb> pixels,
                                            const std::size_t width,
                                            const std::size_t height) noexcept -> bool {
    return width != 0 && height != 0 && pixels.size() / width == height &&
           pixels.size() % width == 0;
}

[[nodiscard]] inline auto rgb_bytes(const Span<const color::Rgb> pixels) -> std::string {
    std::string bytes;
    if (pixels.size() <= bytes.max_size() / 3) bytes.reserve(pixels.size() * 3);
    for (const auto pixel : pixels) {
        bytes.push_back(static_cast<char>(pixel.red));
        bytes.push_back(static_cast<char>(pixel.green));
        bytes.push_back(static_cast<char>(pixel.blue));
    }
    return bytes;
}

[[nodiscard]] inline auto base64(const std::string_view bytes) -> std::string {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    const auto groups = bytes.size() / 3 + (bytes.size() % 3 != 0 ? 1 : 0);
    if (groups <= encoded.max_size() / 4) encoded.reserve(groups * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const auto a = static_cast<unsigned char>(bytes[offset]);
        const auto b = offset + 1 < bytes.size() ? static_cast<unsigned char>(bytes[offset + 1]) : 0U;
        const auto c = offset + 2 < bytes.size() ? static_cast<unsigned char>(bytes[offset + 2]) : 0U;
        encoded.push_back(alphabet[a >> 2]);
        encoded.push_back(alphabet[((a & 0x03U) << 4) | (b >> 4)]);
        encoded.push_back(offset + 1 < bytes.size() ? alphabet[((b & 0x0fU) << 2) | (c >> 6)] : '=');
        encoded.push_back(offset + 2 < bytes.size() ? alphabet[c & 0x3fU] : '=');
    }
    return encoded;
}

inline void append_le(std::string& bytes, std::uint32_t value, const unsigned size) {
    for (unsigned i = 0; i < size; ++i, value >>= 8) bytes += static_cast<char>(value & 255);
}

// iTerm2 expects an encoded image file, not unframed RGB samples.
[[nodiscard]] inline auto bmp(const Span<const color::Rgb> pixels, const std::size_t width,
                              const std::size_t height) -> std::string {
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    if (width > (max - 3U) / 3U || height > max / 2U) return {};
    const auto stride = (width * 3 + 3) & ~std::size_t{3};
    if (height > (max - 54U) / stride) return {};
    const auto size = static_cast<std::uint32_t>(height * stride);
    std::string bytes{"BM"};
    append_le(bytes, 54 + size, 4);
    append_le(bytes, 0, 4);
    append_le(bytes, 54, 4);
    append_le(bytes, 40, 4);
    append_le(bytes, static_cast<std::uint32_t>(width), 4);
    append_le(bytes, static_cast<std::uint32_t>(height), 4);
    append_le(bytes, 1, 2);
    append_le(bytes, 24, 2);
    append_le(bytes, 0, 4);
    append_le(bytes, size, 4);
    bytes.append(16, '\0');
    for (auto row = height; row-- != 0;) {
        for (std::size_t col = 0; col < width; ++col) {
            const auto pixel = pixels[row * width + col];
            bytes += static_cast<char>(pixel.blue);
            bytes += static_cast<char>(pixel.green);
            bytes += static_cast<char>(pixel.red);
        }
        bytes.append(stride - width * 3, '\0');
    }
    return bytes;
}

}  // namespace detail

/** Sixel RGB renderer with up to 256 palette registers. */
struct Sixel {
    // WezTerm escape sequences: DCS [params] q data ST is Sixel graphic data.
    [[nodiscard]] static inline auto encode(const Span<const color::Rgb> pixels,
                                            const std::size_t width, const std::size_t height)
        -> std::string {
        if (!detail::dimensions_match(pixels, width, height)) return {};
        std::vector<color::Rgb> palette;
        std::unordered_map<unsigned, unsigned> index;
        std::vector<unsigned char> samples;
        samples.reserve(pixels.size());
        const auto collect = [&](const bool reduce) {
            for (auto pixel : pixels) {
                if (reduce) {
                    // Fixed RGB332 reduction bounds the palette to 256 registers.
                    pixel.red = static_cast<std::uint8_t>((pixel.red >> 5) * 255U / 7U);
                    pixel.green = static_cast<std::uint8_t>((pixel.green >> 5) * 255U / 7U);
                    pixel.blue = static_cast<std::uint8_t>((pixel.blue >> 6) * 255U / 3U);
                }
                const auto key = (static_cast<unsigned>(pixel.red) << 16U) |
                                 (static_cast<unsigned>(pixel.green) << 8U) | pixel.blue;
                const auto [it, inserted] = index.try_emplace(key, palette.size());
                if (inserted) {
                    if (palette.size() == 256) return false;
                    palette.push_back(pixel);
                }
                samples.push_back(static_cast<unsigned char>(it->second));
            }
            return true;
        };
        if (!collect(false)) {
            palette.clear();
            index.clear();
            samples.clear();
            collect(true);
        }

        std::string out = "\x1bPq\"1;1;" + std::to_string(width) + ';' + std::to_string(height);
        for (std::size_t i = 0; i < palette.size(); ++i) {
            const auto p = palette[i];
            out += '#' + std::to_string(i) + ";2;" + std::to_string((p.red * 100U + 127) / 255U) +
                   ';' + std::to_string((p.green * 100U + 127) / 255U) + ';' +
                   std::to_string((p.blue * 100U + 127) / 255U);
        }
        for (std::size_t band = 0; band < height;) {
            const auto rows = (height - band < 6) ? height - band : 6;
            std::array<std::size_t, 256> ends{};
            for (std::size_t y = 0; y < rows; ++y)
                for (std::size_t x = 0; x < width; ++x) {
                    auto& end = ends[samples[(band + y) * width + x]];
                    if (end <= x) end = x + 1;
                }
            bool first = true;
            for (std::size_t i = 0; i < palette.size(); ++i) {
                if (ends[i] == 0) continue;
                if (!first) out += '$';
                first = false;
                out += '#' + std::to_string(i);
                for (std::size_t x = 0; x < ends[i]; ++x) {
                    unsigned mask = 0;
                    for (std::size_t y = 0; y < rows; ++y)
                        if (samples[(band + y) * width + x] == i) mask |= 1U << y;
                    out += static_cast<char>('?' + mask);
                }
            }
            band += rows;
            if (band != height) out += '-';
        }
        return out + "\x1b\\";
    }
};

/** Kitty direct RGB renderer with payload chunking. */
struct Kitty {
    // kitty terminal graphics protocol, "The graphics escape code":
    // APC_G action=transmit, format=RGB, medium=direct, payload ST.
    [[nodiscard]] static inline auto encode(const Span<const color::Rgb> pixels,
                                            const std::size_t width, const std::size_t height)
        -> std::string {
        if (!detail::dimensions_match(pixels, width, height)) return {};
        const auto payload = detail::base64(detail::rgb_bytes(pixels));
        std::string out;
        for (std::size_t offset = 0; offset < payload.size();) {
            const auto count = std::min<std::size_t>(4096, payload.size() - offset);
            out += "\x1b_G";
            if (offset == 0)
                out += "a=T,f=24,s=" + std::to_string(width) + ",v=" + std::to_string(height) + ',';
            out += (offset + count < payload.size()) ? "m=1;" : "m=0;";
            out.append(payload, offset, count);
            out += "\x1b\\";
            offset += count;
        }
        return out;
    }
};

/** iTerm2 inline-file renderer, using BMP for raw RGB inputs. */
struct ITerm2 {
    /** Wraps an existing image file such as a PNG or JPEG. */
    [[nodiscard]] static inline auto encode_file(const std::string_view bytes) -> std::string {
        if (bytes.empty()) return {};
        return "\x1b]1337;File=inline=1;size=" + std::to_string(bytes.size()) +
               ':' + detail::base64(bytes) + "\x07";
    }

    // WezTerm escape sequences: OSC 1337 is iTerm2's inline-file protocol.
    [[nodiscard]] static inline auto encode(const Span<const color::Rgb> pixels,
                                            const std::size_t width, const std::size_t height)
        -> std::string {
        if (!detail::dimensions_match(pixels, width, height)) return {};
        return encode_file(detail::bmp(pixels, width, height));
    }
};

[[nodiscard]] inline auto render_to_seq(const Sixel&, const Span<const color::Rgb> pixels,
                                        const std::size_t width, const std::size_t height) -> std::string {
    return Sixel::encode(pixels, width, height);
}

[[nodiscard]] inline auto render_to_seq(const Kitty&, const Span<const color::Rgb> pixels,
                                        const std::size_t width, const std::size_t height) -> std::string {
    return Kitty::encode(pixels, width, height);
}

[[nodiscard]] inline auto render_to_seq(const ITerm2&, const Span<const color::Rgb> pixels,
                                        const std::size_t width, const std::size_t height) -> std::string {
    return ITerm2::encode(pixels, width, height);
}

}  // namespace ttytk::image

namespace ttytk {
using Image = image::Image;
}  // namespace ttytk
