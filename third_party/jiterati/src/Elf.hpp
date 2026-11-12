/** @file Elf.hpp @brief Internal bounded ELF external-layout reader.
 * Layout reference: lapislazuli/.info/binutils/include/elf/external.h.
 * No host-layout casts, unchecked offset sums, or relocation application.
 */
#ifndef JITERATI_INTERNAL_ELF_HPP
#define JITERATI_INTERNAL_ELF_HPP
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati::native::detail {
class ElfImage {
public:
    struct Section {
        std::uint64_t name, type, flags, offset, size, link, info, entsize;
    };
    explicit ElfImage(std::vector<std::uint8_t> const& bytes) : bytes_(bytes) {
        require(bytes.size() >= 16, "truncated identification");
        require(bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F', "bad magic");
        require(bytes[4] == 1 || bytes[4] == 2, "bad class");
        require(bytes[5] == 1 || bytes[5] == 2, "bad byte order");
        is64 = bytes[4] == 2; little = bytes[5] == 1;
        require(bytes.size() >= (is64 ? 64u : 52u), "truncated header");
        require(bytes[6] == 1 && get(20, 4) == 1, "bad version");
        type = get(16, 2); machine = get(18, 2);
        require(type >= 1 && type <= 4, "bad file type");
        require(get(is64 ? 52 : 40, 2) == (is64 ? 64u : 52u), "bad header size");
        shoff_ = get(is64 ? 40 : 32, is64 ? 8 : 4);
        shstride_ = get(is64 ? 58 : 46, 2);
        shnum_ = get(is64 ? 60 : 48, 2); shstr_ = get(is64 ? 62 : 50, 2);
        auto phoff = get(is64 ? 32 : 28, is64 ? 8 : 4);
        auto phstride = get(is64 ? 54 : 42, 2), phnum = get(is64 ? 56 : 44, 2);
        if (!shoff_) {
            require(!shnum_ && !shstr_ && phnum != 0xffff, "missing section table");
        } else {
            require(shstride_ == (is64 ? 64u : 40u) && span(shoff_, 1, shstride_), "bad section table");
            auto zero = section_at(shoff_);
            require(zero.type == 0, "section zero must be null");
            if (!shnum_) shnum_ = zero.size;
            if (shstr_ == 0xffff) shstr_ = zero.link;
            if (phnum == 0xffff) phnum = zero.info;
            require(shnum_ && shstr_ < shnum_ && span(shoff_, shnum_, shstride_), "bad section count/index");
        }
        if (phnum) {
            require(phoff && phstride == (is64 ? 56u : 32u) && span(phoff, phnum, phstride), "bad program table");
            for (std::uint64_t i = 0; i < phnum; ++i) {
                auto p = phoff + i * phstride;
                auto off = get(p + (is64 ? 8 : 4), is64 ? 8 : 4);
                auto size = get(p + (is64 ? 32 : 16), is64 ? 8 : 4);
                require(span(off, size, 1), "bad segment span");
            }
        }
        for (std::uint64_t i = 0; i < shnum_; ++i) {
            auto s = section(i);
            if (s.type != 0 && s.type != 8) require(span(s.offset, s.size, 1), "bad section span");
        }
        if (shstr_) require(section(shstr_).type == 3, "section names are not a string table");
    }

    std::uint64_t get(std::uint64_t off, unsigned size) const {
        require(size <= 8 && span(off, size, 1), "truncated field");
        std::uint64_t value = 0;
        for (unsigned i = 0; i < size; ++i)
            value |= std::uint64_t(bytes_[static_cast<std::size_t>(off) + i]) << (8 * (little ? i : size - 1 - i));
        return value;
    }
    bool span(std::uint64_t off, std::uint64_t count, std::uint64_t stride) const {
        return off <= bytes_.size() && stride && count <= (bytes_.size() - static_cast<std::size_t>(off)) / stride;
    }
    std::uint64_t count() const { return shnum_; }
    Section section(std::uint64_t index) const {
        require(index < shnum_, "bad section index");
        return section_at(shoff_ + index * shstride_);
    }
    std::string_view string(Section const& table, std::uint64_t index) const {
        require(table.type == 3 && span(table.offset, table.size, 1) && index < table.size, "bad string index");
        auto start = static_cast<std::size_t>(table.offset + index);
        auto end = start;
        auto limit = static_cast<std::size_t>(table.offset + table.size);
        while (end < limit && bytes_[end]) ++end;
        require(end < limit, "unterminated string");
        return {reinterpret_cast<char const*>(bytes_.data() + start), end - start};
    }
    std::string_view name(Section const& s) const {
        require(shstr_ != 0, "missing section names");
        return string(section(shstr_), s.name);
    }
    bool has_start() const {
        for (std::uint64_t i = 0; i < count(); ++i) {
            auto symbols = section(i);
            if (symbols.type != 2) continue;
            auto stride = is64 ? 24u : 16u;
            require(symbols.entsize == stride && symbols.size % stride == 0, "bad symbol table size");
            auto strings = section(symbols.link);
            for (std::uint64_t j = 0; j < symbols.size / stride; ++j) {
                auto p = symbols.offset + j * stride;
                auto info = get(p + (is64 ? 4 : 12), 1);
                auto index = get(p + (is64 ? 6 : 14), 2);
                if (string(strings, get(p, 4)) != "_start" || (info >> 4) == 0 || !index || index >= count()) continue;
                auto code = section(index);
                if (code.type == 1 && (code.flags & 4) && get(p + (is64 ? 8 : 4), is64 ? 8 : 4) < code.size) return true;
            }
        }
        return false;
    }
    bool is64 = false, little = true;
    std::uint64_t type = 0, machine = 0;

private:
    static void require(bool condition, char const* message) {
        if (!condition) throw std::invalid_argument(std::string("ELF: ") + message);
    }
    Section section_at(std::uint64_t p) const {
        if (is64) return {get(p,4),get(p+4,4),get(p+8,8),get(p+24,8),get(p+32,8),get(p+40,4),get(p+44,4),get(p+56,8)};
        return {get(p,4),get(p+4,4),get(p+8,4),get(p+16,4),get(p+20,4),get(p+24,4),get(p+28,4),get(p+36,4)};
    }
    std::vector<std::uint8_t> const& bytes_;
    std::uint64_t shoff_ = 0, shstride_ = 0, shnum_ = 0, shstr_ = 0;
};
} // namespace jiterati::native::detail
#endif
