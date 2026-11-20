#include "riscv64_cpu.hpp"
#include <bit>
#include <sstream>

namespace domvirt::virtualization {
namespace {
using U = std::uint64_t;
using S = std::int64_t;
__extension__ typedef unsigned __int128 Wide;
U sext(U value, unsigned bits) {
    const U sign = U{1} << (bits - 1);
    return ((value & ((sign << 1) - 1)) ^ sign) - sign;
}
S signed_value(U value) { return std::bit_cast<S>(value); }
U arithmetic_shift(U value, unsigned count) { return U(signed_value(value) >> count); }
void require(bool valid, const char* reason = "reserved or unsupported RV64 encoding") {
    if (!valid) throw std::runtime_error(reason);
}
U high_product(U a, U b, bool signed_a, bool signed_b) {
    U high = U((Wide(a) * Wide(b)) >> 64);
    if (signed_a && (a >> 63)) high -= b;
    if (signed_b && (b >> 63)) high -= a;
    return high;
}
U divide(U a, U b, bool is_signed, bool remainder) {
    if (!b) return remainder ? a : UINT64_MAX;
    if (!is_signed) return remainder ? a % b : a / b;
    if (a == (U{1} << 63) && b == UINT64_MAX) return remainder ? 0 : a;
    return U(remainder ? signed_value(a) % signed_value(b) : signed_value(a) / signed_value(b));
}
}

Riscv64Cpu::Riscv64Cpu(hypervisor::MemoryManager& memory, const machine::Profile& profile,
                       U entry, U argument, Serial serial)
    : memory_(memory), profile_(profile), serial_(std::move(serial)) {
    state_.pc = entry;
    state_.x[2] = profile.stack_top;
    state_.x[10] = argument; // a0: DomIOS boot information, not a Linux/SBI hart ID
}

U Riscv64Cpu::load(U address, unsigned bytes) {
    require(!(address & (bytes - 1)), "misaligned data read");
    if (address <= profile_.serial_base && profile_.serial_base - address < bytes) {
        require(address == profile_.serial_base && bytes == 1, "serial requires byte access");
        return serial_ ? serial_(false, 0) : 0;
    }
    auto span = memory_.get_span(address, bytes);
    require(span.size() == bytes, "read outside guest RAM");
    U value = 0;
    for (unsigned i = 0; i < bytes; ++i) value |= U(span[i]) << (8 * i);
    return value;
}
void Riscv64Cpu::store(U address, unsigned bytes, U value) {
    require(!(address & (bytes - 1)), "misaligned data write");
    if (address <= profile_.serial_base && profile_.serial_base - address < bytes) {
        require(address == profile_.serial_base && bytes == 1, "serial requires byte access");
        if (serial_) serial_(true, static_cast<std::uint8_t>(value));
        return;
    }
    auto span = memory_.get_span(address, bytes);
    require(span.size() == bytes, "write outside guest RAM");
    for (unsigned i = 0; i < bytes; ++i) span[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

VmResult Riscv64Cpu::step() {
    if (halted_) return {};
    const auto before = state_;
    std::uint32_t word = 0;
    try {
        require(!(state_.pc & 3), "misaligned instruction address (IALIGN=32)");
        const auto bytes = memory_.get_span(state_.pc, 4);
        require(bytes.size() == 4, "instruction fetch outside guest RAM");
        for (unsigned i = 0; i < 4; ++i) word |= std::uint32_t(bytes[i]) << (8 * i);
        require((word & 3) == 3, "compressed instructions are not enabled");
        if (!decoded_.contains(word)) {
            sandstone::archml::runtime::Decoded decoded;
            require(profile_.isa.decode(bytes, decoded), "instruction absent from ArchML profile");
            if (decoded_.size() < 65536) decoded_.insert(word);
        }
        execute(word);
        return {};
    } catch (const std::exception& error) {
        state_ = before;
        std::ostringstream message;
        message << "RISC-V at 0x" << std::hex << before.pc << " (0x" << word << "): " << error.what();
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, message.str()};
    }
}

void Riscv64Cpu::execute(std::uint32_t w) {
    const unsigned op = w & 127, rd = (w >> 7) & 31, f3 = (w >> 12) & 7;
    const unsigned rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31, f7 = w >> 25;
    const U a = state_.x[rs1], b = state_.x[rs2], pc = state_.pc;
    U next = pc + 4, value = 0;
    bool writes = true;
    auto target = [&](U address) {
        require(!(address & 3), "misaligned branch target (IALIGN=32)");
        next = address;
    };
    switch (op) {
    case 0x37: value = sext(w & 0xfffff000u, 32); break; // LUI sign extends on RV64
    case 0x17: value = pc + sext(w & 0xfffff000u, 32); break;
    case 0x6f: {
        const U imm = ((w >> 31) << 20) | (((w >> 12) & 255) << 12) |
                      (((w >> 20) & 1) << 11) | (((w >> 21) & 1023) << 1);
        target(pc + sext(imm, 21)); value = pc + 4; break;
    }
    case 0x67:
        require(f3 == 0);
        target((a + sext(w >> 20, 12)) & ~U{1}); value = pc + 4; break;
    case 0x63: {
        bool taken;
        switch (f3) {
        case 0: taken = a == b; break;
        case 1: taken = a != b; break;
        case 4: taken = signed_value(a) < signed_value(b); break;
        case 5: taken = signed_value(a) >= signed_value(b); break;
        case 6: taken = a < b; break;
        case 7: taken = a >= b; break;
        default: throw std::runtime_error("reserved branch encoding");
        }
        const U imm = ((w >> 31) << 12) | (((w >> 7) & 1) << 11) |
                      (((w >> 25) & 63) << 5) | (((w >> 8) & 15) << 1);
        if (taken) target(pc + sext(imm, 13));
        writes = false; break;
    }
    case 0x03: {
        require(f3 != 7);
        const unsigned bytes = 1u << (f3 & 3);
        value = load(a + sext(w >> 20, 12), bytes); // even rd=x0 must perform/fault the read
        if (f3 < 3) value = sext(value, bytes * 8);
        break;
    }
    case 0x23:
        require(f3 <= 3);
        store(a + sext(((w >> 25) << 5) | ((w >> 7) & 31), 12), 1u << f3, b);
        writes = false; break;
    case 0x13: case 0x1b: {
        const bool word = op == 0x1b;
        const U imm = sext(w >> 20, 12);
        const unsigned amount = (w >> 20) & (word ? 31 : 63);
        if (f3 == 0) value = a + imm;
        else if (f3 == 1) {
            require(word ? f7 == 0 : (w >> 26) == 0);
            value = a << amount;
        } else if (f3 == 5) {
            const unsigned mode = word ? f7 : w >> 26;
            require(mode == 0 || mode == (word ? 0x20u : 0x10u));
            value = mode ? arithmetic_shift(word ? sext(a, 32) : a, amount) :
                           (word ? a & UINT32_MAX : a) >> amount;
        } else {
            require(!word);
            switch (f3) {
            case 2: value = signed_value(a) < signed_value(imm); break;
            case 3: value = a < imm; break;
            case 4: value = a ^ imm; break;
            case 6: value = a | imm; break;
            case 7: value = a & imm; break;
            default: throw std::runtime_error("reserved immediate encoding");
            }
        }
        if (word) value = sext(value, 32);
        break;
    }
    case 0x33: case 0x3b: {
        const bool word = op == 0x3b;
        const unsigned amount = b & (word ? 31 : 63);
        if (f7 == 1) {
            if (f3 == 0) value = a * b;
            else if (f3 < 4) {
                require(!word);
                value = high_product(a, b, f3 != 3, f3 == 1);
            } else {
                const bool sign = !(f3 & 1);
                value = divide(word ? (sign ? sext(a, 32) : a & UINT32_MAX) : a,
                               word ? (sign ? sext(b, 32) : b & UINT32_MAX) : b, sign, f3 >= 6);
            }
        } else {
            require(f7 == 0 || (f7 == 0x20 && (f3 == 0 || f3 == 5)));
            require(!word || f3 == 0 || f3 == 1 || f3 == 5);
            switch (f3) {
            case 0: value = f7 ? a - b : a + b; break;
            case 1: value = a << amount; break;
            case 2: value = signed_value(a) < signed_value(b); break;
            case 3: value = a < b; break;
            case 4: value = a ^ b; break;
            case 5: value = f7 ? arithmetic_shift(word ? sext(a, 32) : a, amount) :
                                (word ? a & UINT32_MAX : a) >> amount; break;
            case 6: value = a | b; break;
            case 7: value = a & b; break;
            }
        }
        if (word) value = sext(value, 32);
        break;
    }
    case 0x0f:
        // A single synchronous hart has no outstanding memory/device work or
        // instruction cache. Reserved operand fields are ignored as specified.
        require(f3 == 0 || f3 == 1, "unsupported memory-order instruction");
        writes = false; break;
    case 0x73:
        require(w == 0x00100073, "unsupported SYSTEM instruction (no SBI or privileged trap state)");
        halted_ = true; writes = false; break; // DomIOS EBREAK terminal convention
    default: throw std::runtime_error("unsupported RV64 opcode");
    }
    if (writes && rd) state_.x[rd] = value;
    state_.x[0] = 0;
    state_.pc = next;
}
} // namespace domvirt::virtualization
