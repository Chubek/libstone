#pragma once

#include <TTyUtils/TTyUtils-ScrollBar.hpp>
#include <TTyUtils/TTyUtils-Widget.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ttyutils::hexview {

struct Region {
    std::size_t begin{};
    std::size_t end{};
};

struct Annotation {
    Region region{};
    std::string name{};
    theme::Style style{};
};

class Inspector {
public:
    [[nodiscard]] static auto u16(const std::vector<std::uint8_t>& bytes, const std::size_t offset,
                                  const std::endian order = std::endian::little) -> std::uint16_t {
        return static_cast<std::uint16_t>(read(bytes, offset, 2, order).value_or(0));
    }
    [[nodiscard]] static auto u32(const std::vector<std::uint8_t>& bytes, const std::size_t offset,
                                  const std::endian order = std::endian::little) -> std::uint32_t {
        return static_cast<std::uint32_t>(read(bytes, offset, 4, order).value_or(0));
    }
    /// Inspect up to eight bytes, distinguishing truncated input from a zero value.
    [[nodiscard]] static auto read(const std::span<const std::uint8_t> bytes, const std::size_t offset,
                                   const std::size_t count, const std::endian order = std::endian::little)
        -> std::optional<std::uint64_t> {
        if (offset > bytes.size() || !count || count > 8 || count > bytes.size() - offset) return {};
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = order == std::endian::big ? i : count - 1 - i;
            result = (result << 8U) | bytes[offset + index];
        }
        return result;
    }
};

class HexView : public ui::Widget, public ui::Scrollable {
public:
    [[nodiscard]] auto open(const std::filesystem::path& path) -> bool {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) return false;
        set_data(std::move(data));
        return true;
    }
    void set_data(std::vector<std::uint8_t> data) {
        data_ = std::move(data); cursor_ = top_ = 0; invalidate(rect());
    }
    void annotate(Annotation annotation) { annotations_.push_back(std::move(annotation)); invalidate(rect()); }
    void annotate(const Region region, std::string name, const theme::Style& style = {}) {
        annotate({region, std::move(name), style});
    }
    [[nodiscard]] auto data() const noexcept -> const std::vector<std::uint8_t>& { return data_; }
    [[nodiscard]] auto annotations() const noexcept -> const std::vector<Annotation>& { return annotations_; }
    [[nodiscard]] auto offset() const -> std::size_t override { return std::min(top_, max_offset()); }
    [[nodiscard]] auto length() const -> std::size_t override {
        return data_.size() / columns + (data_.size() % columns != 0);
    }
    [[nodiscard]] auto visible_length() const -> std::size_t override {
        return static_cast<std::size_t>(std::max(rect().height, 1));
    }
    void scroll_to(const std::size_t row) override { top_ = std::min(row, max_offset()); invalidate(rect()); }
    [[nodiscard]] auto cursor_offset() const noexcept -> std::size_t { return cursor_; }
    void set_cursor(const std::size_t byte) {
        cursor_ = data_.empty() ? 0 : std::min(byte, data_.size() - 1);
        const auto row = cursor_ / columns;
        const auto first = offset();
        if (row < first) scroll_to(row);
        else if (row - first >= visible_length()) scroll_to(row - visible_length() + 1);
        else invalidate(rect());
    }
    /// Searches raw bytes from `start`; on a match, moves the shared hex/ASCII cursor.
    [[nodiscard]] auto find(const std::span<const std::uint8_t> needle, const std::size_t start = 0)
        -> std::optional<std::size_t> {
        if (start >= data_.size() || needle.empty()) return {};
        const auto it = std::search(data_.begin() + static_cast<std::ptrdiff_t>(start), data_.end(),
                                    needle.begin(), needle.end());
        if (it == data_.end()) return {};
        set_cursor(static_cast<std::size_t>(it - data_.begin()));
        return cursor_;
    }
    auto handle(const ttytk::input::Event& event) -> bool override {
        const auto* key = std::get_if<ttytk::input::Key>(&event);
        if (!key || key->event_type == 3 || data_.empty()) return false;
        switch (key->codepoint) {
        case 57350: set_cursor(cursor_ ? cursor_ - 1 : 0); return true;
        case 57351: set_cursor(cursor_ + 1); return true;
        case 57352: set_cursor(cursor_ >= columns ? cursor_ - columns : cursor_); return true;
        case 57353: set_cursor(cursor_ + std::min(columns, data_.size() - 1 - cursor_)); return true;
        case 57356: set_cursor(cursor_ - cursor_ % columns); return true;
        case 57357:
            set_cursor(cursor_ + std::min(columns - 1 - cursor_ % columns, data_.size() - 1 - cursor_));
            return true;
        default: return false;
        }
    }

    void paint(ui::Canvas& canvas) override {
        constexpr char digits[] = "0123456789abcdef";
        constexpr int label_width = sizeof(std::size_t) * 2;
        constexpr int hex_x = label_width + 2;
        constexpr int ascii_x = hex_x + static_cast<int>(columns) * 3 + 1;
        const auto area = rect();
        for (int y = 0; y < area.height && offset() + static_cast<std::size_t>(y) < length(); ++y) {
            const auto begin = (offset() + static_cast<std::size_t>(y)) * columns;
            char buffer[label_width];
            const auto out = std::to_chars(buffer, buffer + label_width, begin, 16);
            const auto count = static_cast<std::size_t>(out.ptr - buffer);
            canvas.text(area.x, area.y + y, std::string(label_width - count, '0') + std::string(buffer, count), style());
            const auto bytes = std::min(columns, data_.size() - begin);
            for (std::size_t i = 0; i < bytes; ++i) {
                const auto byte = data_[begin + i];
                auto color = style();
                for (const auto& annotation : annotations_) {
                    if (begin + i >= annotation.region.begin && begin + i < annotation.region.end) {
                        color = annotation.style;
                        break;
                    }
                }
                if (begin + i == cursor_) color.attributes |= 1U << 6;
                const char hex[]{digits[byte >> 4U], digits[byte & 15U]};
                const char ascii = byte >= 0x20 && byte <= 0x7e ? static_cast<char>(byte) : '.';
                canvas.text(area.x + hex_x + static_cast<int>(3 * i), area.y + y, {hex, 2}, color);
                canvas.text(area.x + ascii_x + static_cast<int>(i), area.y + y, {&ascii, 1}, color);
            }
        }
    }

private:
    static constexpr std::size_t columns = 16;
    std::vector<std::uint8_t> data_;
    std::vector<Annotation> annotations_;
    std::size_t cursor_{};
    std::size_t top_{};
};

}  // namespace ttyutils::hexview
