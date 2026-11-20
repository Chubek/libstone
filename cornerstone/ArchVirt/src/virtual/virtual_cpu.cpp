#include "virtual_cpu.hpp"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>

#include "bindlet_bridge/bridge.hpp"

namespace domvirt::virtualization {
namespace {

constexpr std::uint64_t FLAG_CF = 1ull << 0;
constexpr std::uint64_t FLAG_PF = 1ull << 2;
constexpr std::uint64_t FLAG_AF = 1ull << 4;
constexpr std::uint64_t FLAG_ZF = 1ull << 6;
constexpr std::uint64_t FLAG_SF = 1ull << 7;
constexpr std::uint64_t FLAG_DF = 1ull << 10;
constexpr std::uint64_t FLAG_OF = 1ull << 11;

unsigned width_bytes(unsigned bits) {
    return std::max(1u, (bits + 7u) / 8u);
}

std::uint64_t width_mask(unsigned width) {
    if (width >= 8) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return (1ull << (width * 8u)) - 1ull;
}

bool is_relative_operand(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative;
}

bool has_prefix(const ZydisDecodedInstruction& instruction, std::uint8_t value) {
    for (std::uint8_t i = 0; i < instruction.raw.prefix_count; ++i) {
        if (instruction.raw.prefixes[i].value == value) {
            return true;
        }
    }
    return false;
}

} // namespace

VirtualCpu::VirtualCpu(hypervisor::MemoryManager& memory, VirtualCpuConfig config)
    : memory_(memory), config_(config), rip_(config.rip), cr0_(config.cr0),
      cr3_(config.cr3), cr4_(config.cr4), efer_(config.efer) {
    gpr_[4] = config.rsp;
    gpr_[7] = config.boot_argument;
}

VirtualCpu::~VirtualCpu() {
    stop();
    join();
    delete static_cast<ZydisDecoder*>(decoder_storage_);
}

VmResult VirtualCpu::initialize() {
    if (!memory_.translate(config_.rip)) {
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                       "Virtual CPU entry point is outside guest memory");
    }
    if (config_.rsp < 8 || !memory_.translate(config_.rsp - 8)) {
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                       "Virtual CPU stack is outside guest memory");
    }

    auto* decoder = new ZydisDecoder{};
    const auto status = ZydisDecoderInit(decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                         ZYDIS_STACK_WIDTH_64);
    if (!ZYAN_SUCCESS(status)) {
        delete decoder;
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                       "Zydis long-mode decoder initialization failed");
    }
    delete static_cast<ZydisDecoder*>(decoder_storage_);
    decoder_storage_ = decoder;
    initialized_.store(true);
    halted_.store(false);
    stop_requested_.store(false);
    instruction_count_.store(0);
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        result_ = {VmErrorCode::SUCCESS, ""};
    }
    return {VmErrorCode::SUCCESS, "Virtual CPU initialized"};
}

VmResult VirtualCpu::run() {
    if (!initialized_.load()) {
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Virtual CPU is not initialized"};
    }
    if (running_.exchange(true)) {
        return {VmErrorCode::SUCCESS, "Virtual CPU already running"};
    }
    stop_requested_.store(false);
    halted_.store(false);
    thread_ = std::make_unique<std::jthread>(&VirtualCpu::execution_loop, this);
    return {VmErrorCode::SUCCESS, "Virtual CPU thread started"};
}

void VirtualCpu::stop() {
    stop_requested_.store(true);
}

void VirtualCpu::join() {
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

VmResult VirtualCpu::result() const {
    std::lock_guard<std::mutex> lock(result_mutex_);
    return result_;
}

VcpuRegisters VirtualCpu::registers() const {
    VcpuRegisters regs{};
    regs.rax = gpr_[0]; regs.rcx = gpr_[1]; regs.rdx = gpr_[2]; regs.rbx = gpr_[3];
    regs.rsp = gpr_[4]; regs.rbp = gpr_[5]; regs.rsi = gpr_[6]; regs.rdi = gpr_[7];
    regs.r8 = gpr_[8]; regs.r9 = gpr_[9]; regs.r10 = gpr_[10]; regs.r11 = gpr_[11];
    regs.r12 = gpr_[12]; regs.r13 = gpr_[13]; regs.r14 = gpr_[14]; regs.r15 = gpr_[15];
    regs.rip = rip_; regs.rflags = rflags_;
    regs.cr0 = cr0_; regs.cr3 = cr3_; regs.cr4 = cr4_; regs.efer = efer_;
    return regs;
}

void VirtualCpu::execution_loop() {
    while (!stop_requested_.load() && !halted_.load()) {
        auto result = step();
        if (!result) {
            std::lock_guard<std::mutex> lock(result_mutex_);
            result_ = result;
            break;
        }
        if ((instruction_count_.load() & 0x3FFFu) == 0u) {
            std::this_thread::yield();
        }
    }
    running_.store(false);
}

VmResult VirtualCpu::step() {
    if (!initialized_.load()) {
        return {VmErrorCode::VIRTUAL_BACKEND_ERROR, "Virtual CPU is not initialized"};
    }
    if (halted_.load()) return {};
    const auto old_rip = rip_, old_flags = rflags_;
    std::array<std::uint64_t, 16> old_registers;
    std::copy(std::begin(gpr_), std::end(gpr_), old_registers.begin());
    auto result = execute_instruction();
    if (!result) {
        rip_ = old_rip; rflags_ = old_flags;
        std::copy(old_registers.begin(), old_registers.end(), std::begin(gpr_));
    }
    return result;
}

VmResult VirtualCpu::execute_instruction() {
    auto* decoder = static_cast<ZydisDecoder*>(decoder_storage_);
    if (!decoder) {
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Zydis decoder is unavailable");
    }

    const auto total_ram = static_cast<std::uint64_t>(memory_.total_ram());
    if (rip_ >= total_ram) {
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                       "Instruction fetch outside guest memory at 0x" +
                       std::to_string(rip_));
    }
    const auto fetch_length = static_cast<std::size_t>(
        std::min<std::uint64_t>(15, total_ram - rip_));
    auto bytes = memory_.get_span(rip_, fetch_length);
    if (bytes.empty()) {
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                       "Instruction fetch outside guest memory at 0x" +
                       std::to_string(rip_));
    }
    ZydisDecodedInstruction instruction{};
    std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands{};
    const auto status = ZydisDecoderDecodeFull(
        decoder, bytes.data(), bytes.size(), &instruction, operands.data());
    if (!ZYAN_SUCCESS(status)) {
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                       "Zydis failed to decode instruction at 0x" +
                       std::to_string(rip_));
    }

    const std::uint64_t instruction_address = rip_;
    const std::uint64_t next_rip = rip_ + instruction.length;
    rip_ = next_rip;
    const auto* ops = operands.data();
    auto binary_width = [&]() {
        return width_bytes(instruction.operand_count_visible > 0
                               ? operands[0].size
                               : instruction.operand_width);
    };
    auto imm_target = [&](unsigned index = 0u) {
        return branch_target(&ops[index], next_rip);
    };
    auto stack_push = [&](std::uint64_t value) -> bool {
        if (gpr_[4] < 8 || !write_memory(gpr_[4] - 8, 8, value)) {
            return false;
        }
        gpr_[4] -= 8;
        return true;
    };
    auto stack_pop = [&](std::uint64_t& value) -> bool {
        if (!read_memory(gpr_[4], 8, value)) {
            return false;
        }
        gpr_[4] += 8;
        return true;
    };

    auto condition = [&](unsigned code) {
        bool value = false;
        switch (code >> 1) {
        case 0: value = flag(FLAG_OF); break;
        case 1: value = flag(FLAG_CF); break;
        case 2: value = flag(FLAG_ZF); break;
        case 3: value = flag(FLAG_CF) || flag(FLAG_ZF); break;
        case 4: value = flag(FLAG_SF); break;
        case 5: value = flag(FLAG_PF); break;
        case 6: value = flag(FLAG_SF) != flag(FLAG_OF); break;
        case 7: value = flag(FLAG_ZF) || (flag(FLAG_SF) != flag(FLAG_OF)); break;
        }
        return (code & 1) ? !value : value;
    };
    if (instruction.meta.category == ZYDIS_CATEGORY_CMOV || instruction.meta.category == ZYDIS_CATEGORY_SETCC) {
        const bool selected = condition(instruction.opcode & 15);
        std::uint64_t value = selected;
        if (instruction.meta.category == ZYDIS_CATEGORY_CMOV &&
            !read_operand(&ops[1], instruction_address, instruction.length, value))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "CMOV source read failed");
        if ((instruction.meta.category == ZYDIS_CATEGORY_SETCC || selected) &&
            !write_operand(&ops[0], instruction_address, instruction.length, value))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Conditional write failed");
    } else if (instruction.meta.category == ZYDIS_CATEGORY_COND_BR &&
               ((instruction.opcode >= 0x70 && instruction.opcode <= 0x7f) ||
                (instruction.opcode >= 0x80 && instruction.opcode <= 0x8f))) {
        if (condition(instruction.opcode & 15)) rip_ = imm_target();
    } else switch (instruction.mnemonic) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_CLD:
    case ZYDIS_MNEMONIC_CLI:
    case ZYDIS_MNEMONIC_CLC:
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CLD) {
            rflags_ &= ~FLAG_DF;
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_CLC) {
            rflags_ &= ~FLAG_CF;
        }
        break;
    case ZYDIS_MNEMONIC_HLT:
        halted_.store(true);
        break;
    case ZYDIS_MNEMONIC_MOV: {
        std::uint64_t value = 0;
        if (!read_operand(&ops[1], instruction_address, instruction.length, value) ||
            !write_operand(&ops[0], instruction_address, instruction.length, value)) {
            std::ostringstream message;
            message << "MOV accessed invalid guest memory at 0x" << std::hex
                    << instruction_address;
            if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                std::uint64_t address = 0;
                (void)operand_address(&ops[1], instruction_address,
                                      instruction.length, address);
                message << " effective=0x" << address
                        << " base=" << static_cast<unsigned>(ops[1].mem.base)
                        << " index=" << static_cast<unsigned>(ops[1].mem.index)
                        << " disp=" << ops[1].mem.disp.value
                        << " r8=0x" << gpr_[8]
                        << " r10=0x" << gpr_[10]
                        << " r12=0x" << gpr_[12];
            }
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, message.str());
        }
        break;
    }
    case ZYDIS_MNEMONIC_MOVZX:
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD: {
        std::uint64_t value = 0;
        if (!read_operand(&ops[1], instruction_address, instruction.length, value))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Extending move read failed");
        if (instruction.mnemonic != ZYDIS_MNEMONIC_MOVZX) {
            const auto sign = 1ull << (ops[1].size - 1);
            value = (value ^ sign) - sign;
        }
        if (!write_operand(&ops[0], instruction_address, instruction.length, value))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Extending move write failed");
        break;
    }
    case ZYDIS_MNEMONIC_LEA: {
        std::uint64_t address = 0;
        if (!operand_address(&ops[1], instruction_address, instruction.length, address) ||
            !write_operand(&ops[0], instruction_address, instruction.length, address)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "LEA accessed invalid guest memory");
        }
        break;
    }
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_ADC:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR: {
        std::uint64_t lhs = 0, rhs = 0;
        if (!read_operand(&ops[0], instruction_address, instruction.length, lhs) ||
            !read_operand(&ops[1], instruction_address, instruction.length, rhs)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "ALU read accessed invalid guest memory");
        }
        const unsigned width = binary_width();
        const auto mask = width_mask(width);
        lhs &= mask;
        rhs &= mask;
        std::uint64_t value = 0;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_ADD || instruction.mnemonic == ZYDIS_MNEMONIC_ADC) {
            const auto carry = instruction.mnemonic == ZYDIS_MNEMONIC_ADC && flag(FLAG_CF);
            value = (lhs + rhs + carry) & mask;
            set_add_flags(lhs, rhs, value, width);
            set_flag(FLAG_CF, lhs > mask - rhs || (carry && lhs == mask - rhs));
        } else if (instruction.mnemonic == ZYDIS_MNEMONIC_SUB || instruction.mnemonic == ZYDIS_MNEMONIC_SBB) {
            const auto borrow = instruction.mnemonic == ZYDIS_MNEMONIC_SBB && flag(FLAG_CF);
            value = (lhs - rhs - borrow) & mask;
            set_sub_flags(lhs, rhs, value, width);
            set_flag(FLAG_CF, lhs < rhs || (borrow && lhs == rhs));
        } else {
            value = (instruction.mnemonic == ZYDIS_MNEMONIC_AND ? lhs & rhs :
                     instruction.mnemonic == ZYDIS_MNEMONIC_OR ? lhs | rhs : lhs ^ rhs) & mask;
            set_logic_flags(value, width);
        }
        if (!write_operand(&ops[0], instruction_address, instruction.length, value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "ALU write accessed invalid guest memory");
        }
        break;
    }
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        std::uint64_t value = 0;
        if (!read_operand(&ops[0], instruction_address, instruction.length, value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "INC/DEC read accessed invalid guest memory");
        }
        const unsigned width = width_bytes(ops[0].size);
        const auto mask = width_mask(width);
        const bool old_cf = flag(FLAG_CF);
        const std::uint64_t result = instruction.mnemonic == ZYDIS_MNEMONIC_INC
            ? (value + 1u) & mask
            : (value - 1u) & mask;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_INC) {
            set_add_flags(value & mask, 1, result, width);
        } else {
            set_sub_flags(value & mask, 1, result, width);
        }
        set_flag(FLAG_CF, old_cf);
        if (!write_operand(&ops[0], instruction_address, instruction.length, result)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "INC/DEC write accessed invalid guest memory");
        }
        break;
    }
    case ZYDIS_MNEMONIC_CMP:
    case ZYDIS_MNEMONIC_TEST: {
        std::uint64_t lhs = 0, rhs = 0;
        if (!read_operand(&ops[0], instruction_address, instruction.length, lhs) ||
            !read_operand(&ops[1], instruction_address, instruction.length, rhs)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "CMP/TEST accessed invalid guest memory");
        }
        const unsigned width = binary_width();
        const auto mask = width_mask(width);
        lhs &= mask;
        rhs &= mask;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_CMP) {
            set_sub_flags(lhs, rhs, (lhs - rhs) & mask, width);
        } else {
            set_logic_flags(lhs & rhs, width);
        }
        break;
    }
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR: {
        std::uint64_t value, count;
        if (!read_operand(&ops[0], instruction_address, instruction.length, value) ||
            !read_operand(&ops[1], instruction_address, instruction.length, count))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Shift read failed");
        const unsigned width = binary_width(), bits = width * 8;
        count &= bits == 64 ? 63 : 31;
        if (!count) break;
        const auto original = value & width_mask(width);
        const bool negative = (original >> (bits - 1)) != 0;
        bool carry = false;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_SHL) {
            carry = count <= bits && ((original >> (bits - count)) & 1);
            value = count >= bits ? 0 : original << count;
        } else {
            carry = count <= bits ? (original >> (count - 1)) & 1 :
                    instruction.mnemonic == ZYDIS_MNEMONIC_SAR && negative;
            if (instruction.mnemonic == ZYDIS_MNEMONIC_SAR) {
                const auto sign = 1ull << (bits - 1);
                const auto signed_value = std::bit_cast<std::int64_t>(std::uint64_t((original ^ sign) - sign));
                value = static_cast<std::uint64_t>(signed_value >> std::min<std::uint64_t>(count, 63));
            } else value = count >= bits ? 0 : original >> count;
        }
        set_logic_flags(value, width); set_flag(FLAG_CF, carry);
        if (count == 1) set_flag(FLAG_OF, instruction.mnemonic == ZYDIS_MNEMONIC_SHL
            ? bool((value >> (bits - 1)) & 1) != carry : instruction.mnemonic == ZYDIS_MNEMONIC_SHR && negative);
        if (!write_operand(&ops[0], instruction_address, instruction.length, value))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Shift write failed");
        break;
    }
    case ZYDIS_MNEMONIC_IMUL:
    case ZYDIS_MNEMONIC_MUL: {
        __extension__ using Wide = unsigned __int128;
        __extension__ using SignedWide = __int128;
        std::uint64_t a, b;
        const unsigned width = binary_width(), bits = width * 8;
        bool unary = instruction.operand_count_visible == 1;
        if (!read_operand(&ops[unary ? 0 : instruction.operand_count_visible - 2], instruction_address, instruction.length, b))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Multiply read failed");
        if (unary) a = gpr_[0] & width_mask(width);
        else if (!read_operand(&ops[instruction.operand_count_visible - 1], instruction_address, instruction.length, a))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Multiply read failed");
        Wide product;
        bool overflow;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_IMUL) {
            auto sign = 1ull << (bits - 1);
            auto sa = std::bit_cast<std::int64_t>(std::uint64_t(((a & width_mask(width)) ^ sign) - sign));
            auto sb = std::bit_cast<std::int64_t>(std::uint64_t(((b & width_mask(width)) ^ sign) - sign));
            auto signed_product = SignedWide(sa) * SignedWide(sb);
            product = static_cast<Wide>(signed_product);
            auto low_signed = std::bit_cast<std::int64_t>(std::uint64_t(((std::uint64_t(product) & width_mask(width)) ^ sign) - sign));
            overflow = signed_product != low_signed;
        } else { product = Wide(a) * Wide(b); overflow = (product >> bits) != 0; }
        if (unary) {
            if (width == 1) write_register({0, 2, false, true}, static_cast<std::uint64_t>(product));
            else {
                write_register({0, width, false, true}, static_cast<std::uint64_t>(product));
                write_register({2, width, false, true}, static_cast<std::uint64_t>(product >> bits));
            }
        } else if (!write_operand(&ops[0], instruction_address, instruction.length, static_cast<std::uint64_t>(product)))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Multiply write failed");
        set_flag(FLAG_CF, overflow); set_flag(FLAG_OF, overflow);
        break;
    }
    case ZYDIS_MNEMONIC_NEG:
    case ZYDIS_MNEMONIC_NOT: {
        std::uint64_t value;
        if (!read_operand(&ops[0], instruction_address, instruction.length, value))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Unary read failed");
        auto result = instruction.mnemonic == ZYDIS_MNEMONIC_NEG ? 0 - value : ~value;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_NEG) set_sub_flags(0, value, result, binary_width());
        if (!write_operand(&ops[0], instruction_address, instruction.length, result))
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "Unary write failed");
        break;
    }
    case ZYDIS_MNEMONIC_JMP:
        rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_JZ:
        if (flag(FLAG_ZF)) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_JNZ:
        if (!flag(FLAG_ZF)) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_JB:
        if (flag(FLAG_CF)) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_JBE:
        if (flag(FLAG_CF) || flag(FLAG_ZF)) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_JNB:
        if (!flag(FLAG_CF)) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_JNBE:
        if (!flag(FLAG_CF) && !flag(FLAG_ZF)) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_CALL:
        if (!stack_push(next_rip)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "CALL stack write failed");
        }
        rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_RET: {
        std::uint64_t target = 0;
        if (!stack_pop(target)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "RET stack read failed");
        }
        rip_ = target;
        break;
    }
    case ZYDIS_MNEMONIC_PUSH: {
        std::uint64_t value = 0;
        if (!read_operand(&ops[0], instruction_address, instruction.length, value) ||
            !stack_push(value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "PUSH accessed invalid guest memory");
        }
        break;
    }
    case ZYDIS_MNEMONIC_POP: {
        std::uint64_t value = 0;
        if (!stack_pop(value) ||
            !write_operand(&ops[0], instruction_address, instruction.length, value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "POP accessed invalid guest memory");
        }
        break;
    }
    case ZYDIS_MNEMONIC_LOOP:
        --gpr_[1];
        if (gpr_[1] != 0) rip_ = imm_target();
        break;
    case ZYDIS_MNEMONIC_LODSB: {
        std::uint64_t value = 0;
        if (!read_memory(gpr_[6], 1, value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "LODSB read failed");
        }
        gpr_[0] = (gpr_[0] & ~0xFFull) | value;
        gpr_[6] += flag(FLAG_DF) ? static_cast<std::uint64_t>(-1) : 1u;
        break;
    }
    case ZYDIS_MNEMONIC_MOVSB: {
        const bool repeated = has_prefix(instruction, 0xF3);
        const std::uint64_t count = repeated ? gpr_[1] : 1u;
        for (std::uint64_t i = 0; i < count; ++i) {
            std::uint64_t value = 0;
            if (!read_memory(gpr_[6], 1, value) ||
                !write_memory(gpr_[7], 1, value)) {
                return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                               "MOVSB accessed invalid guest memory");
            }
            const auto delta = flag(FLAG_DF)
                ? static_cast<std::uint64_t>(-1)
                : 1u;
            gpr_[6] += delta;
            gpr_[7] += delta;
        }
        if (repeated) {
            gpr_[1] = 0;
        }
        break;
    }
    case ZYDIS_MNEMONIC_STOSW: {
        const bool repeated = has_prefix(instruction, 0xF3);
        const std::uint64_t count = repeated ? gpr_[1] : 1u;
        const std::uint16_t value = static_cast<std::uint16_t>(gpr_[0]);
        for (std::uint64_t i = 0; i < count; ++i) {
            if (!write_memory(gpr_[7], 2, value)) {
                return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "STOSW write failed");
            }
            gpr_[7] += flag(FLAG_DF) ? static_cast<std::uint64_t>(-2) : 2u;
        }
        if (repeated) {
            gpr_[1] = 0;
        }
        break;
    }
    case ZYDIS_MNEMONIC_IN: {
        std::uint64_t port = 0;
        if (!read_operand(&ops[1], instruction_address, instruction.length, port)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "IN port read failed");
        }
        const auto value = io_handler_ ? io_handler_(static_cast<std::uint16_t>(port),
                                                     false, 1, 0) : 0;
        if (!write_operand(&ops[0], instruction_address, instruction.length, value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "IN destination write failed");
        }
        break;
    }
    case ZYDIS_MNEMONIC_OUT: {
        std::uint64_t port = 0;
        std::uint64_t value = 0;
        if (!read_operand(&ops[0], instruction_address, instruction.length, port) ||
            !read_operand(&ops[1], instruction_address, instruction.length, value)) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "OUT operand read failed");
        }
        if (bridge_) {
            bindlet_bridge::BridgeEvent event{};
            event.type = bindlet_bridge::BridgeEventType::IO_ACCESS;
            event.address = port;
            event.value = value;
            event.size = 1;
            event.is_write = true;
            event.vcpu_id = config_.vcpu_id;
            bridge_->dispatch_event(event);
        }
        if (io_handler_) {
            io_handler_(static_cast<std::uint16_t>(port), true, 1,
                        static_cast<std::uint8_t>(value));
        }
        break;
    }
    case ZYDIS_MNEMONIC_DIV: {
        std::uint64_t divisor = 0;
        if (!read_operand(&ops[0], instruction_address, instruction.length, divisor) ||
            divisor == 0) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "DIV divisor is invalid");
        }
        const unsigned width = width_bytes(ops[0].size);
        if (width == 8 && gpr_[2] != 0) {
            return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR,
                           "64-bit DIV with a non-zero high dividend is unsupported");
        } else {
            const std::uint64_t dividend = gpr_[0];
            const auto quotient = dividend / divisor;
            const auto remainder = dividend % divisor;
            if (width == 4 && quotient > std::numeric_limits<std::uint32_t>::max()) {
                return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, "DIV quotient overflow");
            }
            gpr_[0] = quotient;
            gpr_[2] = remainder;
        }
        break;
    }
    default: {
        std::ostringstream message;
        message << "Unsupported guest instruction "
                << ZydisMnemonicGetString(instruction.mnemonic)
                << " (opcode 0x" << std::hex << static_cast<unsigned>(instruction.opcode)
                << ", byte 0x" << static_cast<unsigned>(bytes[0])
                << ") at 0x" << instruction_address;
        return failure(VmErrorCode::VIRTUAL_BACKEND_ERROR, message.str());
    }
    }

    instruction_count_.fetch_add(1);
    return {VmErrorCode::SUCCESS, ""};
}

VirtualCpu::RegisterRef VirtualCpu::register_ref(std::uint32_t reg) const {
    const auto value = static_cast<ZydisRegister>(reg);
    const auto klass = ZydisRegisterGetClass(value);
    const auto id = ZydisRegisterGetId(value);
    if (id < 0) {
        return {};
    }
    if (klass == ZYDIS_REGCLASS_GPR16 || klass == ZYDIS_REGCLASS_GPR32 ||
        klass == ZYDIS_REGCLASS_GPR64) {
        return {static_cast<unsigned>(id),
                klass == ZYDIS_REGCLASS_GPR16 ? 2u :
                klass == ZYDIS_REGCLASS_GPR32 ? 4u : 8u, false, true};
    }
    if (klass == ZYDIS_REGCLASS_GPR8) {
        switch (value) {
        case ZYDIS_REGISTER_AL: return {0, 1, false, true};
        case ZYDIS_REGISTER_CL: return {1, 1, false, true};
        case ZYDIS_REGISTER_DL: return {2, 1, false, true};
        case ZYDIS_REGISTER_BL: return {3, 1, false, true};
        case ZYDIS_REGISTER_AH: return {0, 1, true, true};
        case ZYDIS_REGISTER_CH: return {1, 1, true, true};
        case ZYDIS_REGISTER_DH: return {2, 1, true, true};
        case ZYDIS_REGISTER_BH: return {3, 1, true, true};
        case ZYDIS_REGISTER_SPL: return {4, 1, false, true};
        case ZYDIS_REGISTER_BPL: return {5, 1, false, true};
        case ZYDIS_REGISTER_SIL: return {6, 1, false, true};
        case ZYDIS_REGISTER_DIL: return {7, 1, false, true};
        case ZYDIS_REGISTER_R8B: return {8, 1, false, true};
        case ZYDIS_REGISTER_R9B: return {9, 1, false, true};
        case ZYDIS_REGISTER_R10B: return {10, 1, false, true};
        case ZYDIS_REGISTER_R11B: return {11, 1, false, true};
        case ZYDIS_REGISTER_R12B: return {12, 1, false, true};
        case ZYDIS_REGISTER_R13B: return {13, 1, false, true};
        case ZYDIS_REGISTER_R14B: return {14, 1, false, true};
        case ZYDIS_REGISTER_R15B: return {15, 1, false, true};
        default: return {};
        }
    }
    if (value == ZYDIS_REGISTER_RIP) {
        return {0, 8, false, true};
    }
    return {};
}

std::uint64_t VirtualCpu::read_register(const RegisterRef& reg) const {
    if (!reg.valid) return 0;
    if (reg.width == 8) return gpr_[reg.index];
    if (reg.high8) return (gpr_[reg.index] >> 8) & 0xFFu;
    return gpr_[reg.index] & width_mask(reg.width);
}

void VirtualCpu::write_register(const RegisterRef& reg, std::uint64_t value) {
    if (!reg.valid) return;
    if (reg.width == 8) {
        gpr_[reg.index] = value;
        return;
    }
    if (reg.high8) {
        gpr_[reg.index] = (gpr_[reg.index] & ~0xFF00ull) | ((value & 0xFFu) << 8);
    } else if (reg.width == 4) {
        gpr_[reg.index] = static_cast<std::uint32_t>(value);
    } else if (reg.width == 2) {
        gpr_[reg.index] = (gpr_[reg.index] & ~0xFFFFull) | (value & 0xFFFFu);
    } else {
        gpr_[reg.index] = (gpr_[reg.index] & ~0xFFull) | (value & 0xFFu);
    }
}

bool VirtualCpu::read_memory(std::uint64_t address, unsigned width,
                             std::uint64_t& value) const {
    const auto span = memory_.get_span(address, width);
    if (span.empty()) return false;
    value = 0;
    std::memcpy(&value, span.data(), width);
    return true;
}

bool VirtualCpu::write_memory(std::uint64_t address, unsigned width,
                              std::uint64_t value) {
    const auto span = memory_.get_span(address, width);
    if (span.empty()) return false;
    std::memcpy(span.data(), &value, width);
    return true;
}

bool VirtualCpu::operand_address(const void* raw_operand,
                                 std::uint64_t instruction_address,
                                 std::uint8_t instruction_length,
                                 std::uint64_t& address) const {
    const auto& operand = *static_cast<const ZydisDecodedOperand*>(raw_operand);
    if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) return false;
    address = operand.mem.disp.value;
    if (operand.mem.base == ZYDIS_REGISTER_RIP) {
        address += instruction_address + instruction_length;
    } else if (operand.mem.base != ZYDIS_REGISTER_NONE) {
        const auto base = register_ref(operand.mem.base);
        if (!base.valid) return false;
        address += read_register(base);
    }
    if (operand.mem.index != ZYDIS_REGISTER_NONE) {
        const auto index = register_ref(operand.mem.index);
        if (!index.valid) return false;
        address += read_register(index) * operand.mem.scale;
    }
    return true;
}

bool VirtualCpu::read_operand(const void* raw_operand,
                              std::uint64_t instruction_address,
                              std::uint8_t instruction_length,
                              std::uint64_t& value) const {
    const auto& operand = *static_cast<const ZydisDecodedOperand*>(raw_operand);
    switch (operand.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        const auto reg = register_ref(operand.reg.value);
        if (operand.reg.value == ZYDIS_REGISTER_RIP) {
            value = rip_;
            return true;
        }
        if (!reg.valid) return false;
        value = read_register(reg);
        return true;
    }
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        value = operand.imm.is_signed ? static_cast<std::uint64_t>(operand.imm.value.s)
                                      : operand.imm.value.u;
        return true;
    case ZYDIS_OPERAND_TYPE_MEMORY: {
        std::uint64_t address = 0;
        if (!operand_address(raw_operand, instruction_address, instruction_length, address)) {
            return false;
        }
        return read_memory(address, width_bytes(operand.size), value);
    }
    default:
        return false;
    }
}

bool VirtualCpu::write_operand(const void* raw_operand,
                               std::uint64_t instruction_address,
                               std::uint8_t instruction_length,
                               std::uint64_t value) {
    const auto& operand = *static_cast<const ZydisDecodedOperand*>(raw_operand);
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        if (operand.reg.value == ZYDIS_REGISTER_RIP) return false;
        const auto reg = register_ref(operand.reg.value);
        if (!reg.valid) return false;
        write_register(reg, value);
        return true;
    }
    if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        std::uint64_t address = 0;
        if (!operand_address(raw_operand, instruction_address, instruction_length, address)) {
            return false;
        }
        return write_memory(address, width_bytes(operand.size), value);
    }
    return false;
}

std::uint64_t VirtualCpu::branch_target(const void* raw_operand,
                                        std::uint64_t next_rip) const {
    const auto& operand = *static_cast<const ZydisDecodedOperand*>(raw_operand);
    if (is_relative_operand(operand)) {
        return next_rip + static_cast<std::int64_t>(operand.imm.value.s);
    }
    std::uint64_t value = 0;
    if (!read_operand(raw_operand, next_rip, 0, value)) return UINT64_MAX;
    return value;
}

void VirtualCpu::set_logic_flags(std::uint64_t value, unsigned width) {
    const auto mask = width_mask(width);
    const auto result = value & mask;
    set_flag(FLAG_ZF, result == 0);
    set_flag(FLAG_PF, (std::popcount(static_cast<unsigned>(result & 255)) & 1) == 0);
    set_flag(FLAG_SF, (result & (1ull << (width * 8u - 1u))) != 0);
    set_flag(FLAG_CF, false);
    set_flag(FLAG_OF, false);
}

void VirtualCpu::set_add_flags(std::uint64_t lhs, std::uint64_t rhs,
                               std::uint64_t result, unsigned width) {
    const auto mask = width_mask(width);
    const auto sign = 1ull << (width * 8u - 1u);
    set_flag(FLAG_CF, (lhs & mask) > mask - (rhs & mask));
    set_flag(FLAG_PF, (std::popcount(static_cast<unsigned>(result & 255)) & 1) == 0);
    set_flag(FLAG_AF, ((lhs ^ rhs ^ result) & 16) != 0);
    set_flag(FLAG_ZF, (result & mask) == 0);
    set_flag(FLAG_SF, (result & sign) != 0);
    set_flag(FLAG_OF, ((~(lhs ^ rhs) & (lhs ^ result) & sign) != 0));
}

void VirtualCpu::set_sub_flags(std::uint64_t lhs, std::uint64_t rhs,
                               std::uint64_t result, unsigned width) {
    const auto mask = width_mask(width);
    const auto sign = 1ull << (width * 8u - 1u);
    set_flag(FLAG_CF, (lhs & mask) < (rhs & mask));
    set_flag(FLAG_PF, (std::popcount(static_cast<unsigned>(result & 255)) & 1) == 0);
    set_flag(FLAG_AF, ((lhs ^ rhs ^ result) & 16) != 0);
    set_flag(FLAG_ZF, (result & mask) == 0);
    set_flag(FLAG_SF, (result & sign) != 0);
    set_flag(FLAG_OF, (((lhs ^ rhs) & (lhs ^ result) & sign) != 0));
}

void VirtualCpu::set_flag(std::uint64_t mask, bool value) noexcept {
    if (value) {
        rflags_ |= mask;
    } else {
        rflags_ &= ~mask;
    }
}

VmResult VirtualCpu::failure(VmErrorCode code, std::string message) {
    std::lock_guard<std::mutex> lock(result_mutex_);
    result_ = {code, std::move(message)};
    return result_;
}

} // namespace domvirt::virtualization
