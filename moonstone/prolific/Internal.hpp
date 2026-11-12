#pragma once
#include "Prolific.hpp"

#include <limits>
#include <stdexcept>

namespace moonstone::prolific::detail {
void validate_report(const Report &report);
inline bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80)
            continue;
        unsigned n = 0, value = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 1;
            value = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2;
            value = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3;
            value = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (n > text.size() - i)
            return false;
        while (n--) {
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
inline bool valid(Domain d) { return static_cast<unsigned>(d) <= 2; }
inline bool valid(Kind k) { return static_cast<unsigned>(k) <= 6; }
inline bool severity(std::string_view s) { return s == "note" || s == "warning" || s == "error"; }
inline void increment(std::uint64_t &value) {
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
inline bool add(std::uint64_t &value, std::uint64_t n) {
    if (n > std::numeric_limits<std::uint64_t>::max() - value) {
        value = std::numeric_limits<std::uint64_t>::max();
        return false;
    }
    value += n;
    return true;
}
} // namespace moonstone::prolific::detail
