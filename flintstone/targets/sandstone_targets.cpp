#include "SandstoneTargets.hpp"
#include "../internal.hpp"

namespace flintstone {
namespace {

struct RawArch {
    const char* key, *arch, *subarch, *abi, *obj_format, *endian;
    unsigned addr_size;
};

constexpr RawArch kArchitectures[] = {
    {"aarch64", "aarch64", "armv8-a", "AAPCS64", "ELF64", "little", 64},
    {"amd64", "amd64", "x86-64", "SysV", "ELF64", "little", 64},
    {"amdgpu", "amdgpu", "gfx900", "amdgpu-HSA", "ELF64", "little", 64},
    {"arm32", "arm32", "armv7-a", "AAPCS32", "ELF32", "little", 32},
    {"avr", "avr", "avr5", "AVR-GCC", "ELF32", "little", 16},
    {"bpf", "bpf", "bpfel", "eBPF", "ELF64", "little", 64},
    {"directx", "directx", "dxil", "DXIL", "dxil", "little", 64},
    {"hexagon", "hexagon", "v68", "Hexagon", "ELF32", "little", 32},
    {"lanai", "lanai", "lanai", "Lanai32", "ELF32", "big", 32},
    {"loongarch32", "loongarch32", "la32", "ILP32D", "ELF32", "little", 32},
    {"loongarch64", "loongarch64", "la64", "LP64D", "ELF64", "little", 64},
    {"m68k", "m68k", "68020", "SystemV", "ELF32", "big", 32},
    {"mips32", "mips32", "mips32r2", "o32", "ELF32", "little", 32},
    {"mips64", "mips64", "mips64r2", "n64", "ELF64", "little", 64},
    {"msp430", "msp430", "msp430", "TI-EABI", "ELF32", "little", 16},
    {"nvptx", "nvptx", "sm_80", "NVPTX", "ptx", "little", 64},
    {"ppc32", "ppc32", "powerpc", "SysV", "ELF32", "big", 32},
    {"ppc64", "ppc64", "power9", "ELFv2", "ELF64", "little", 64},
    {"riscv32", "riscv32", "rv32gcv", "ILP32D", "ELF32", "little", 32},
    {"riscv64", "riscv64", "rv64gcv", "LP64D", "ELF64", "little", 64},
    {"s390x", "s390x", "z/arch", "s390x-ELF", "ELF64", "big", 64},
    {"sparc64", "sparc64", "v9", "SPARC-V9", "ELF64", "big", 64},
    {"spirv", "spirv", "spirv1.6", "spirv-logical", "spirv", "little", 32},
    {"tricore", "tricore", "tc1.6", "TriCore-EABI", "ELF32", "little", 32},
    {"wasm", "wasm", "wasm32", "wasm-stack", "wasm", "little", 32},
    {"x86", "x86", "i386", "SysV", "ELF32", "little", 32},
    {"xcore", "xcore", "xs1", "XCore", "ELF32", "little", 32},
};

std::uint32_t add_form(domsymas::Builder& builder, const char* mnemonic, const char* encoder, unsigned operands,
                       unsigned flags, std::uint32_t unit) {
    auto mid = builder.add_mnemonic(mnemonic, {}, 0);
    auto enc = builder.add_encoder(mnemonic, encoder, 0, 32, 0);
    auto form = builder.add_form(mid, operands, flags, enc, {}, {}, {});
    builder.link_mnemonic_form(mid, form);
    domsymas::SchedRec timing;
    timing.form = form;
    timing.name = mnemonic;
    timing.latency = 1;
    timing.writes = {unit};
    builder.add_sched(timing);
    return form;
}

void add_reg_operand(domsymas::Builder& builder, std::uint32_t form, unsigned position, unsigned access) {
    domsymas::OperandRec operand;
    operand.form = form;
    operand.position = position;
    operand.kind = DOMSYMAS_OP_REG;
    operand.access = access;
    operand.width = 32;
    builder.add_operand(operand);
}

void add_imm_operand(domsymas::Builder& builder, std::uint32_t form, unsigned position, std::int64_t min, std::int64_t max) {
    domsymas::OperandRec operand;
    operand.form = form;
    operand.position = position;
    operand.kind = DOMSYMAS_OP_IMM;
    operand.access = DOMSYMAS_ACC_READ;
    operand.width = 32;
    operand.is_signed = 1;
    operand.imm_min = min;
    operand.imm_max = max;
    builder.add_operand(operand);
}

struct TinySpec {
    const char* arch;
    const char* regs[8];
    unsigned enc[8];
    unsigned reg_count;
    const char* li;
    const char* mov;
    const char* add;
    const char* sub;
    const char* mul;
    const char* ret;
    std::int64_t imm_min, imm_max;
};

const TinySpec& tiny_spec(TinyArch arch) {
    // Encodings are the real machine encodings for these integer subsets:
    // x86-64 MOV r32,imm32 / MOV r,r / ADD+MOV expansion / IMUL / RET,
    // AArch64 MOVZ / ADD / SUB / MUL(wZR-ra) / RET X30,
    // RISC-V ADDI / ADD / SUB / MUL / JALR x0,0(x1).
    static const TinySpec x86 = {
        // Only classic low registers: r8d/r9d would need REX prefixes, and
        // esp/ebp must never be clobbered by generated host-executed code.
        "x86-64",
        {"eax", "ecx", "edx", "ebx", "esi", "edi", "", ""},
        {0, 1, 2, 3, 6, 7, 0, 0},
        6,
        "emit8(0xB8 + dst); emit32le(imm);",
        "emit8(0x89); emit_modrm(src, dst);",
        "require(dst == lhs); emit8(0x89); emit_modrm(lhs, dst); emit8(0x01); emit_modrm(rhs, dst);",
        "require(dst == lhs); emit8(0x89); emit_modrm(lhs, dst); emit8(0x29); emit_modrm(rhs, dst);",
        "require(dst == lhs); emit8(0x89); emit_modrm(lhs, dst); emit8(0x0F); emit8(0xAF); emit_modrm(rhs, dst);",
        "emit8(0xC3);",
        INT64_C(-2147483648),
        INT64_C(2147483647),
    };
    // The SymAS encoding language supports only +, -, *, /, %, comparisons
    // and logical connectives, so bitfields are composed with multiply-add
    // (all fields are disjoint, verified against the ISA manual layouts).
    static const TinySpec aarch64 = {
        "aarch64",
        {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"},
        {0, 1, 2, 3, 4, 5, 6, 7},
        8,
        "require(imm >= 0); require(imm < 65536); emit32le(1384120320 + imm * 32 + dst);",
        "emit32le(184549376 + 31 * 65536 + src * 32 + dst);",
        "emit32le(184549376 + rhs * 65536 + lhs * 32 + dst);",
        "emit32le(1258291200 + rhs * 65536 + lhs * 32 + dst);",
        "emit32le(453016576 + rhs * 65536 + lhs * 32 + dst);",
        "emit32le(3596551104);",
        0,
        65535,
    };
    // RISC-V bases: ADDI opcode 0x13; ADD 0x33; SUB 0x40000033;
    // MUL 0x02000033; RET is JALR x0,0(x1) = 0x8067 = 32871. Signed field
    // arithmetic composes correctly because emit32le truncates two's
    // complement to 32 bits.
    static const TinySpec riscv = {
        "riscv64",
        {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"},
        {10, 11, 12, 13, 14, 15, 16, 17},
        8,
        "require(imm >= -2048); require(imm < 2048); emit32le(imm * 1048576 + dst * 128 + 19);",
        "emit32le(src * 32768 + dst * 128 + 19);",
        "emit32le(51 + rhs * 1048576 + lhs * 32768 + dst * 128);",
        "emit32le(1073741875 + rhs * 1048576 + lhs * 32768 + dst * 128);",
        "emit32le(33554483 + rhs * 1048576 + lhs * 32768 + dst * 128);",
        "emit32le(32871);",
        INT64_C(-2048),
        INT64_C(2047),
    };
    switch (arch) {
    case TinyArch::X86_64: return x86;
    case TinyArch::AArch64: return aarch64;
    case TinyArch::RiscV64: return riscv;
    }
    detail::fail("tiny.arch", "unknown tiny architecture");
}

SelectedFunction select_integer_subset(const Module& module, const Function& function, const Registry& registry,
                                       const domsymas::Databank& bank, const TinyForms& forms, std::uint32_t model, bool two_address) {
    using detail::check;
    check(function.blocks.size() == 1, "tiny.cfg", "tiny integer targets support single-block functions");
    check(function.parameters.empty(), "tiny.params", "tiny integer targets take no parameters (no ABI lowering)");
    SelectedFunction result;
    result.scheduling.name = function.name;
    result.scheduling.entry = 0;
    result.scheduling.blocks = {{0, "entry", {}, {}}};
    limestone::regtl::Block block{};
    block.id = 0;
    std::uint32_t next = 100;
    // The tiny banks contain only GPRs, so the allocation class is every
    // register record, whatever the per-architecture count is.
    std::vector<std::uint32_t> gprs;
    for (const auto& reg : bank.data().registers) gprs.push_back(reg.id);
    result.registers.classes = {{"GPR", gprs}};
    auto emit = [&](std::uint32_t form, std::vector<std::uint32_t> defs, std::vector<std::uint32_t> uses,
                    std::vector<std::pair<std::uint32_t, std::uint32_t>> ties, FormEncoding encoding) {
        auto instruction = sandstone_instruction(bank, form, model, next);
        instruction.defs = defs;
        instruction.uses = uses;
        instruction.ties = ties;
        result.scheduling.instructions.push_back(instruction);
        limestone::regtl::Instruction transfer{};
        transfer.id = next;
        transfer.defs = defs;
        transfer.uses = uses;
        transfer.ties = ties;
        block.instructions.push_back(transfer);
        result.encodings[next] = {form, {}};
        for (auto& binding : encoding.bindings) result.encodings[next].bindings.push_back(binding);
        for (const auto& value : defs) {
            limestone::regtl::VirtualRegister reg{};
            reg.value = value;
            reg.klass = "GPR";
            result.registers.values.push_back(reg);
        }
        ++next;
    };
    auto value_binding = [](const char* name, unsigned position, std::uint32_t id) {
        FormBinding binding;
        binding.name = name;
        binding.position = position;
        binding.kind = FormBinding::Kind::Value;
        binding.id = id;
        return binding;
    };
    auto require_i32 = [&](const Operation& op) {
        for (const auto& value : op.results)
            check(value.type == Type::integer(32), "tiny.type", "tiny integer targets support only i32");
        // Value operands are i32 by transitivity: the module is verified, so
        // every core arithmetic operand matches its (checked i32) result type.
        for (const auto& operand : op.operands)
            if (operand.kind == OperandKind::Integer) {
                check(operand.type == Type::integer(32), "tiny.type", "tiny integer targets support only i32 literals");
            } else if (operand.kind != OperandKind::Value) {
                detail::fail("tiny.operand", "tiny integer targets support only value and integer operands", op.location);
            }
    };
    (void)registry;
    const auto& sole = function.blocks[0];
    for (const auto& op : sole.operations) {
        require_i32(op);
        if (op.name == "arith.constant") {
            check(op.operands.size() == 1 && op.operands[0].kind == OperandKind::Integer, "tiny.constant", "constant requires an integer literal",
                  op.location);
            FormBinding dst = value_binding("dst", 0, op.results[0].id);
            FormBinding imm;
            imm.name = "imm";
            imm.position = 1;
            imm.literal.num = op.operands[0].integer;
            emit(forms.li, {op.results[0].id}, {}, {}, {forms.li, {dst, imm}});
        } else if (op.name == "arith.copy") {
            check(op.operands[0].kind == OperandKind::Value, "tiny.copy", "copy requires a value operand", op.location);
            emit(forms.mov, {op.results[0].id}, {op.operands[0].value}, {},
                 {forms.mov, {value_binding("dst", 0, op.results[0].id), value_binding("src", 1, op.operands[0].value)}});
        } else if (op.name == "arith.add" || op.name == "arith.sub" || op.name == "arith.mul") {
            check(op.operands.size() == 2 && op.operands[0].kind == OperandKind::Value && op.operands[1].kind == OperandKind::Value,
                  "tiny.operands", "binary operation requires two value operands", op.location);
            const auto form = op.name == "arith.add" ? forms.add : op.name == "arith.sub" ? forms.sub : forms.mul;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> ties;
            if (two_address) ties = {{op.results[0].id, op.operands[0].value}};
            emit(form, {op.results[0].id}, {op.operands[0].value, op.operands[1].value}, ties,
                 {form,
                  {value_binding("dst", 0, op.results[0].id), value_binding("lhs", 1, op.operands[0].value),
                   value_binding("rhs", 2, op.operands[1].value)}});
        } else if (op.name == "func.return") {
            auto instruction = sandstone_instruction(bank, forms.ret, model, next);
            instruction.terminator = true;
            instruction.control = limestone::schedrow::ControlFlow::Return;
            if (!op.operands.empty()) {
                check(op.operands[0].kind == OperandKind::Value, "tiny.return", "return requires a value", op.location);
                instruction.uses = {op.operands[0].value};
            }
            result.scheduling.instructions.push_back(instruction);
            limestone::regtl::Instruction transfer{};
            transfer.id = next;
            transfer.defs = {};
            transfer.uses = instruction.uses;
            block.instructions.push_back(transfer);
            FormEncoding encoding{forms.ret, {}};
            if (!op.operands.empty()) encoding.bindings.push_back(value_binding("src", 0, op.operands[0].value));
            result.encodings[next] = std::move(encoding);
            ++next;
        } else {
            detail::fail("tiny.unsupported", "tiny integer targets cannot select '" + op.name + "'", op.location);
        }
    }
    // Deduplicate virtual registers (defs may repeat across ops? No: SSA single
    // definition, but the loop above pushes one entry per def; each SSA value
    // is defined once, so entries are unique by construction).
    result.registers.blocks.push_back(block);
    (void)module;
    return result;
}

} // namespace

struct TinyEntry { const char* name; TinyArch arch; };
constexpr TinyEntry kTinyTargets[] = {
    {"x86-64-tiny", TinyArch::X86_64},
    {"aarch64-tiny", TinyArch::AArch64},
    {"riscv64-tiny", TinyArch::RiscV64},
};

namespace {
const std::vector<ArchInfo>& arch_table() {
    static const std::vector<ArchInfo> table = [] {
        std::vector<ArchInfo> result;
        for (const auto& raw : kArchitectures)
            result.push_back({raw.key, raw.arch, raw.subarch, raw.abi, raw.obj_format, raw.endian, raw.addr_size});
        return result;
    }();
    return table;
}
} // namespace

std::vector<ArchInfo> all_architectures() {
    return arch_table();
}

const ArchInfo* find_architecture(std::string_view key) {
    for (const auto& info : arch_table())
        if (key == info.key) return &info;
    return nullptr;
}

std::string tiny_arch_name(TinyArch arch) {
    for (const auto& entry : kTinyTargets) if (entry.arch == arch) return entry.name;
    detail::fail("tiny.arch", "unknown tiny architecture");
}

bool find_tiny_arch(std::string_view name, TinyArch& arch) {
    for (const auto& entry : kTinyTargets) if (name == entry.name) { arch = entry.arch; return true; }
    return false;
}

std::shared_ptr<domsymas::Databank> make_tiny_bank(TinyArch arch, TinyForms* forms) {
    const auto& spec = tiny_spec(arch);
    auto bank = std::make_shared<domsymas::Databank>();
    domsymas::Builder builder;
    domsymas::TargetRec target;
    target.arch = tiny_arch_name(arch);
    target.addr_size = target.default_addr_size = 64;
    target.default_op_size = 32;
    target.obj_format = "raw";
    target.abi = "tiny";
    target.calling_conv = "tiny";
    builder.add_target(target);
    for (unsigned n = 0; n < spec.reg_count; ++n) {
        domsymas::RegisterRec reg;
        reg.name = spec.regs[n];
        reg.regclass = 0;
        reg.width = 32;
        reg.enc_num = spec.enc[n];
        reg.flags = DOMSYMAS_REG_READABLE | DOMSYMAS_REG_WRITABLE;
        builder.add_register(reg);
    }
    domsymas::ModelRec machine;
    machine.name = "model";
    machine.issue_width = 2;
    const auto model = builder.add_model(machine);
    domsymas::UnitRec unit;
    unit.name = "ALU";
    unit.kind = std::uint32_t(domsymas::UnitKind::Int);
    unit.model = model;
    const auto unit_id = builder.add_unit(unit);
    TinyForms ids;
    ids.li = add_form(builder, "li", spec.li, 2, 0, unit_id);
    ids.mov = add_form(builder, "mov", spec.mov, 2, 0, unit_id);
    ids.add = add_form(builder, "add", spec.add, 3, 0, unit_id);
    ids.sub = add_form(builder, "sub", spec.sub, 3, 0, unit_id);
    ids.mul = add_form(builder, "mul", spec.mul, 3, 0, unit_id);
    ids.ret = add_form(builder, "ret", spec.ret, 1, DOMSYMAS_IF_RETURN | DOMSYMAS_IF_TERMINATOR, unit_id);
    ids.model = model;
    add_reg_operand(builder, ids.li, 0, DOMSYMAS_ACC_WRITE);
    add_imm_operand(builder, ids.li, 1, spec.imm_min, spec.imm_max);
    add_reg_operand(builder, ids.mov, 0, DOMSYMAS_ACC_WRITE);
    add_reg_operand(builder, ids.mov, 1, DOMSYMAS_ACC_READ);
    for (auto form : {ids.add, ids.sub, ids.mul}) {
        add_reg_operand(builder, form, 0, DOMSYMAS_ACC_WRITE);
        add_reg_operand(builder, form, 1, DOMSYMAS_ACC_READ);
        add_reg_operand(builder, form, 2, DOMSYMAS_ACC_READ);
    }
    add_reg_operand(builder, ids.ret, 0, DOMSYMAS_ACC_READ);
    auto status = builder.build(*bank);
    detail::check(status.ok(), "tiny.bank", "tiny bank build failed: " + status.message);
    status = bank->validate(true);
    detail::check(status.ok(), "tiny.bank", "tiny bank validation failed: " + status.message);
    if (forms) *forms = ids;
    return bank;
}

NativeTarget make_tiny_target(TinyArch arch) {
    NativeTarget target;
    TinyForms ids;
    target.bank = make_tiny_bank(arch, &ids);
    target.machine = sandstone_machine(*target.bank, ids.model);
    const bool two_address = arch == TinyArch::X86_64;
    target.select = [bank = target.bank, ids, two_address](const Module& module, const Function& function, const Registry& registry) {
        return select_integer_subset(module, function, registry, *bank, ids, ids.model, two_address);
    };
    return target;
}

std::vector<std::string> tiny_backend_names() {
    std::vector<std::string> names;
    for (const auto& entry : kTinyTargets) names.emplace_back(entry.name);
    return names;
}

void register_tiny_backends(Registry& registry) {
    for (const auto& entry : kTinyTargets) {
        try {
            registry.backend(entry.name);
            continue; // already registered
        } catch (const Error&) {
            // Not registered yet; install below.
        }
        auto target = make_tiny_target(entry.arch);
        registry.add_backend(entry.name, [target = std::move(target)](const Module& module, const Registry& registry) {
            return compile_native(module, registry, target).artifact;
        });
    }
}

} // namespace flintstone
