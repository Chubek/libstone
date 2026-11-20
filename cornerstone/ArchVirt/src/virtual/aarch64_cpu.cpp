#include "aarch64_cpu.hpp"
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>

namespace domvirt::virtualization {
namespace {
using U = std::uint64_t;
U mask(unsigned width) { return width == 64 ? UINT64_MAX : (U{1} << width) - 1; }
U sext(U value, unsigned width) {
    const U sign = U{1} << (width - 1);
    return ((value & mask(width)) ^ sign) - sign;
}
U ror(U value, unsigned count, unsigned width) {
    value &= mask(width); count %= width;
    return count ? ((value >> count) | (value << (width - count))) & mask(width) : value;
}
U shift(U value, unsigned kind, unsigned count, unsigned width) {
    if (count >= width) throw std::runtime_error("reserved shift count");
    value &= mask(width);
    switch (kind) {
    case 0: return (value << count) & mask(width);
    case 1: return value >> count;
    case 2: return static_cast<U>(std::bit_cast<std::int64_t>(sext(value, width)) >> count) & mask(width);
    case 3: return ror(value, count, width);
    default: throw std::runtime_error("invalid shift");
    }
}
void require(bool valid) { if (!valid) throw std::runtime_error("reserved or unsupported A64 encoding"); }
}

Aarch64Cpu::Aarch64Cpu(hypervisor::MemoryManager& memory, const machine::Profile& profile,
                       U entry, U argument, Serial serial)
    : memory_(memory), profile_(profile), serial_(std::move(serial)) {
    state_.pc = entry; state_.sp = profile.stack_top; state_.x[0] = argument;
}

U Aarch64Cpu::reg(unsigned index, bool sp) const {
    return index == 31 ? (sp ? state_.sp : 0) : state_.x[index];
}
void Aarch64Cpu::write(unsigned index, U value, unsigned width, bool sp) {
    value &= mask(width);
    if (index != 31) state_.x[index] = value;
    else if (sp) state_.sp = value;
}
U Aarch64Cpu::load(U address, unsigned bytes) {
    if (address == profile_.serial_base && bytes == 1) return serial_ ? serial_(false, 0) : 0;
    const auto span = memory_.get_span(address, bytes);
    if (span.size() != bytes) throw std::runtime_error("read outside guest RAM");
    U value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= U(span[i]) << (i * 8);
    return value;
}
void Aarch64Cpu::store(U address, unsigned bytes, U value) {
    if (address == profile_.serial_base && bytes == 1) {
        if (serial_) serial_(true, static_cast<std::uint8_t>(value));
        return;
    }
    auto span = memory_.get_span(address, bytes);
    if (span.size() != bytes) throw std::runtime_error("write outside guest RAM");
    for (unsigned i = 0; i < bytes; ++i) span[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void Aarch64Cpu::flags(U a, U b, U result, unsigned width, bool sub) {
    a &= mask(width); b &= mask(width); result &= mask(width);
    const U sign = U{1} << (width - 1);
    const bool carry = sub ? a >= b : result < a;
    const bool overflow = ((sub ? a ^ b : ~(a ^ b)) & (a ^ result) & sign) != 0;
    state_.nzcv = ((result & sign) ? 1u << 31 : 0) | (result == 0 ? 1u << 30 : 0) |
                  (carry ? 1u << 29 : 0) | (overflow ? 1u << 28 : 0);
}
bool Aarch64Cpu::condition(unsigned code) const {
    const bool n = state_.nzcv >> 31, z = (state_.nzcv >> 30) & 1,
               c = (state_.nzcv >> 29) & 1, v = (state_.nzcv >> 28) & 1;
    bool result;
    switch (code >> 1) {
    case 0: result = z; break;
    case 1: result = c; break;
    case 2: result = n; break;
    case 3: result = v; break;
    case 4: result = c && !z; break;
    case 5: result = n == v; break;
    case 6: result = !z && n == v; break;
    default: return true;
    }
    return (code & 1) ? !result : result;
}

VmResult Aarch64Cpu::step() {
    if (halted_) return {};
    const auto before = state_;
    std::uint32_t word = 0;
    try {
        require((state_.pc & 3) == 0);
        const auto bytes = memory_.get_span(state_.pc, 4);
        if (bytes.size() != 4) throw std::runtime_error("instruction fetch outside guest RAM");
        for (unsigned i = 0; i < 4; ++i) word |= std::uint32_t(bytes[i]) << (8 * i);
        if (!decoded_.contains(word)) {
            sandstone::archml::runtime::Decoded decoded;
            if (!profile_.isa.decode(bytes, decoded)) throw std::runtime_error("instruction absent from ArchML profile");
            if (decoded_.size() < 65536) decoded_.insert(word);
        }
        execute(word);
        return {};
    } catch (const std::exception& e) {
        state_ = before;
        std::ostringstream error;
        error << "AArch64 at 0x" << std::hex << before.pc << " (0x" << word << "): " << e.what();
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, error.str()};
    }
}

void Aarch64Cpu::execute(std::uint32_t w) {
    const U pc = state_.pc;
    U next = pc + 4;
    const unsigned width = (w >> 31) ? 64 : 32;
    const unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    auto is = [&](std::uint32_t m, std::uint32_t b) { return (w & m) == b; };
    if (w == 0xd503201f) { /* nop */ }
    else if (w == 0xd4400000) halted_ = true;
    else if (is(0x1f000000, 0x10000000)) {
        U imm = sext(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
        write(rd, (w >> 31) ? (pc & ~U{4095}) + (imm << 12) : pc + imm, 64);
    } else if (is(0x1f800000, 0x12800000)) {
        unsigned op = (w >> 29) & 3, amount = ((w >> 21) & 3) * 16;
        require(op != 1 && amount < width);
        U imm = U((w >> 5) & 65535) << amount;
        write(rd, op == 0 ? ~imm : op == 2 ? imm : (reg(rd) & ~(U{65535} << amount)) | imm, width);
    } else if (is(0x1f000000, 0x11000000) || is(0x1f000000, 0x0b000000)) {
        bool imm = is(0x1f000000, 0x11000000), extended = !imm && (w & (1u << 21));
        bool sub = (w >> 30) & 1, set_flags = (w >> 29) & 1;
        U a = reg(rn, imm || extended), b;
        if (imm) { require(!(w & (1u << 23))); b = U((w >> 10) & 4095) << (((w >> 22) & 1) * 12); }
        else if (extended) {
            unsigned option = (w >> 13) & 7, amount = (w >> 10) & 7;
            require(amount <= 4);
            unsigned bits = 8u << (option & 3);
            b = reg(rm) & mask(bits); if (option & 4) b = sext(b, bits); b <<= amount;
        } else { require(((w >> 22) & 3) != 3); b = shift(reg(rm), (w >> 22) & 3, (w >> 10) & 63, width); }
        U result = sub ? a - b : a + b;
        if (set_flags) flags(a, b, result, width, sub);
        write(rd, result, width, !set_flags && (imm || extended));
    } else if (is(0x1f000000, 0x0a000000) || is(0x1f800000, 0x12000000)) {
        bool imm = is(0x1f800000, 0x12000000);
        U a = reg(rn), b = 0;
        if (imm) {
            unsigned n = (w >> 22) & 1, s = (w >> 10) & 63, r = (w >> 16) & 63;
            unsigned pattern = (n << 6) | ((~s) & 63);
            require(pattern >= 2 && (width == 64 || !n));
            unsigned len = std::bit_width(pattern) - 1, bits = 1u << len;
            unsigned levels = bits - 1;
            require((s & levels) != levels);
            U element = ror(mask((s & levels) + 1), r & levels, bits);
            for (unsigned i = 0; i < width; i += bits) b |= element << i;
        } else {
            b = shift(reg(rm), (w >> 22) & 3, (w >> 10) & 63, width);
            if (w & (1u << 21)) b = ~b;
        }
        unsigned op = (w >> 29) & 3;
        U result = op == 1 ? a | b : op == 2 ? a ^ b : a & b;
        if (op == 3) state_.nzcv = ((result & (U{1} << (width - 1))) ? 1u << 31 : 0) |
                                          ((result & mask(width)) == 0 ? 1u << 30 : 0);
        write(rd, result, width, imm && op != 3);
    } else if (is(0x1f800000, 0x13000000)) {
        unsigned op = (w >> 29) & 3, r = (w >> 16) & 63, s = (w >> 10) & 63;
        require(op != 3 && r < width && s < width && ((w >> 22) & 1) == (width == 64));
        U source = reg(rn) & mask(width), value;
        if (s >= r) {
            unsigned bits = s - r + 1;
            value = (source >> r) & mask(bits);
            if (op == 0) value = sext(value, bits);
            if (op == 1) value |= reg(rd) & ~mask(bits);
        } else {
            unsigned amount = width - r;
            U field_mask = mask(s + 1) << amount;
            value = (source << amount) & field_mask;
            if (op == 0) value = sext(value, s + 1 + amount);
            if (op == 1) value |= reg(rd) & ~field_mask;
        }
        write(rd, value, width);
    } else if (is(0x1f800000, 0x13800000)) {
        unsigned amount = (w >> 10) & 63;
        require(amount < width && !(w & (3u << 29)) && ((w >> 22) & 1) == (width == 64));
        write(rd, amount ? ((reg(rm) & mask(width)) >> amount) | (reg(rn) << (width - amount)) : reg(rm), width);
    } else if (is(0x7c000000, 0x14000000)) {
        if (w >> 31) state_.x[30] = next;
        next = pc + (sext(w & 0x3ffffff, 26) << 2);
    } else if (is(0xff000010, 0x54000000)) {
        if (condition(w & 15)) next = pc + (sext((w >> 5) & 0x7ffff, 19) << 2);
    } else if (is(0x7e000000, 0x34000000)) {
        if (((reg(rd) & mask(width)) != 0) == bool((w >> 24) & 1))
            next = pc + (sext((w >> 5) & 0x7ffff, 19) << 2);
    } else if (is(0x7e000000, 0x36000000)) {
        unsigned bit = ((w >> 19) & 31) | ((w >> 26) & 32);
        if (((reg(rd) >> bit) & 1) == ((w >> 24) & 1)) next = pc + (sext((w >> 5) & 0x3fff, 14) << 2);
    } else if (is(0xfffffc1f, 0xd61f0000) || is(0xfffffc1f, 0xd63f0000) || is(0xfffffc1f, 0xd65f0000)) {
        next = reg(rn);
        if (is(0xfffffc1f, 0xd63f0000)) state_.x[30] = pc + 4;
    } else if (is(0x3e000000, 0x28000000)) {
        unsigned opc = w >> 30, mode = (w >> 23) & 3, rt2 = (w >> 10) & 31;
        bool reading = (w >> 22) & 1;
        require((opc == 0 || opc == 2 || (opc == 1 && reading)) && mode != 0);
        unsigned bytes = opc == 2 ? 8 : 4;
        U base = reg(rn, true), offset = sext((w >> 15) & 127, 7) * bytes;
        U addr = mode == 1 ? base : base + offset;
        // Check both accesses before committing a pair or writeback.
        require(memory_.get_span(addr, bytes * 2).size() == bytes * 2);
        if (reading) {
            U a = load(addr, bytes), b = load(addr + bytes, bytes);
            if (opc == 1) { a = sext(a, 32); b = sext(b, 32); }
            write(rd, a, opc == 1 ? 64 : bytes * 8); write(rt2, b, opc == 1 ? 64 : bytes * 8);
        }
        else { store(addr, bytes, reg(rd)); store(addr + bytes, bytes, reg(rt2)); }
        if (mode != 2) write(rn, base + offset, 64, true);
    } else if (is(0x3f000000, 0x39000000) || is(0x3f200000, 0x38000000) || is(0x3f200c00, 0x38200800)) {
        unsigned size = w >> 30, bytes = 1u << size, opc = (w >> 22) & 3;
        bool unsigned_offset = is(0x3f000000, 0x39000000), reg_offset = is(0x3f200c00, 0x38200800);
        unsigned mode = (w >> 10) & 3;
        U base = reg(rn, true), offset;
        require(!(size == 3 && opc > 1) && !(size == 2 && opc == 3));
        if (unsigned_offset) offset = U((w >> 10) & 4095) * bytes;
        else if (reg_offset) {
            unsigned option = (w >> 13) & 7;
            require(option == 2 || option == 3 || option == 6 || option == 7);
            offset = reg(rm);
            if (option == 2) offset &= mask(32);
            if (option == 6) offset = sext(offset, 32);
            if ((w >> 12) & 1) offset <<= size;
        } else { require(mode != 2); offset = sext((w >> 12) & 511, 9); }
        U addr = !unsigned_offset && !reg_offset && mode == 1 ? base : base + offset;
        if (opc == 0) store(addr, bytes, reg(rd));
        else { U value = load(addr, bytes); if (opc >= 2) value = sext(value, bytes * 8); write(rd, value, opc == 2 || size == 3 ? 64 : 32); }
        if (!unsigned_offset && !reg_offset && (mode == 1 || mode == 3)) write(rn, base + offset, 64, true);
    } else if (is(0x3f000000, 0x18000000)) {
        unsigned opc = w >> 30; require(opc != 3);
        U value = load(pc + (sext((w >> 5) & 0x7ffff, 19) << 2), opc == 1 ? 8 : 4);
        write(rd, opc == 2 ? sext(value, 32) : value, opc == 0 ? 32 : 64);
    } else if (is(0x1fe00000, 0x1a800000)) {
        require(!(w & (1u << 29)) && !(w & (1u << 11)));
        U value = reg(rn);
        if (!condition((w >> 12) & 15)) {
            value = reg(rm);
            if ((w >> 30) & 1) value = ~value;
            if ((w >> 10) & 1) ++value;
        }
        write(rd, value, width);
    } else if (is(0x1fe00000, 0x1a400000)) {
        require((w & (1u << 29)) && !(w & (1u << 4)) && !(w & (1u << 10)));
        if (condition((w >> 12) & 15)) {
            U a = reg(rn), b = (w & (1u << 11)) ? rm : reg(rm);
            bool sub = (w >> 30) & 1;
            flags(a, b, sub ? a - b : a + b, width, sub);
        } else state_.nzcv = (w & 15) << 28;
    } else if (is(0x5fe00000, 0x1ac00000)) {
        require(!(w & (1u << 29)));
        unsigned op = (w >> 10) & 63; U a = reg(rn) & mask(width), b = reg(rm) & mask(width), value;
        if (op == 2) value = b ? a / b : 0;
        else if (op == 3) {
            auto sa = std::bit_cast<std::int64_t>(sext(a, width)), sb = std::bit_cast<std::int64_t>(sext(b, width));
            value = !sb ? 0 : sa == INT64_MIN && sb == -1 ? U(sa) : U(sa / sb);
        } else if (op >= 8 && op <= 11) value = shift(a, op - 8, b & (width - 1), width);
        else throw std::runtime_error("unsupported two-source operation");
        write(rd, value, width);
    } else if (is(0x1f000000, 0x1b000000)) {
        require(!(w & (3u << 29)));
        const unsigned op = (w >> 21) & 7;
        U product = 0, accumulator = reg((w >> 10) & 31);
        if (op == 0) product = reg(rn) * reg(rm);
        else {
            require(width == 64);
            if (op == 1) product = sext(reg(rn), 32) * sext(reg(rm), 32);
            else if (op == 5) product = (reg(rn) & mask(32)) * (reg(rm) & mask(32));
            else if (op == 2 || op == 6) {
                require(((w >> 10) & 31) == 31 && !(w & (1u << 15)));
                __extension__ using Wide = unsigned __int128;
                __extension__ using SignedWide = __int128;
                if (op == 6) product = static_cast<U>((Wide(reg(rn)) * Wide(reg(rm))) >> 64);
                else product = static_cast<U>((SignedWide(std::bit_cast<std::int64_t>(reg(rn))) *
                                                SignedWide(std::bit_cast<std::int64_t>(reg(rm)))) >> 64);
            } else throw std::runtime_error("unsupported multiply variant");
        }
        write(rd, (w & (1u << 15)) ? accumulator - product : accumulator + product, width);
    } else throw std::runtime_error("unsupported A64 execution semantics");
    require((next & 3) == 0);
    state_.pc = next;
}

} // namespace domvirt::virtualization
