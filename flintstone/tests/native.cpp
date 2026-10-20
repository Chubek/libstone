#include "test.hpp"
#include "targets/Native.hpp"
#include "targets/SandstoneTargets.hpp"
#include <cstring>
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace flintstone;
namespace {
struct Fixture {
    std::shared_ptr<domsymas::Databank> bank = std::make_shared<domsymas::Databank>();
    std::uint32_t constant = 0, add = 0, ret = 0, model = 0;
    Fixture() {
        domsymas::Builder builder;
        auto base = builder.add_feature("base", 0, "test architecture");
        auto extra = builder.add_feature("addition", 0, "integer addition");
        domsymas::TargetRec target; target.arch = "stone-test"; target.addr_size = target.default_addr_size = 64;
        target.default_op_size = 32; target.obj_format = "raw"; target.abi = "test"; target.calling_conv = "test"; target.features = {base};
        builder.add_target(target);
        for (unsigned n = 0; n < 3; ++n) {
            domsymas::RegisterRec reg; reg.name = "r" + std::to_string(n); reg.width = 32; reg.enc_num = n;
            reg.flags = DOMSYMAS_REG_READABLE | DOMSYMAS_REG_WRITABLE; builder.add_register(reg);
        }
        domsymas::ModelRec machine; machine.name = "model"; machine.issue_width = 2; model = builder.add_model(machine);
        domsymas::UnitRec unit; unit.name = "ALU"; unit.kind = std::uint32_t(domsymas::UnitKind::Int); unit.model = model;
        auto unit_id = builder.add_unit(unit);
        auto form = [&](const char* name, const char* encoder, unsigned count, unsigned flags, bool needs_extra) {
            auto mnemonic = builder.add_mnemonic(name, {}, 0);
            auto encode = builder.add_encoder(name, encoder, 0, 32, 2);
            auto id = builder.add_form(mnemonic, count, flags, encode, needs_extra ? std::vector<std::uint32_t>{extra} : std::vector<std::uint32_t>{}, {}, {});
            builder.link_mnemonic_form(mnemonic, id);
            domsymas::SchedRec timing; timing.form = id; timing.name = name; timing.latency = 1; timing.writes = {unit_id}; builder.add_sched(timing);
            return id;
        };
        constant = form("li", "emit8(0x10); emit8(dst); emit8(imm);", 2, 0, false);
        add = form("add", "emit8(0x20); emit8(dst); emit8(lhs); emit8(rhs);", 3, 0, true);
        ret = form("ret", "emit8(0x30); emit8(src);", 1, DOMSYMAS_IF_RETURN | DOMSYMAS_IF_TERMINATOR, false);
        auto operand = [&](unsigned form, unsigned position, unsigned kind, unsigned access) {
            domsymas::OperandRec result; result.form = form; result.position = position; result.kind = kind; result.access = access;
            result.width = kind == DOMSYMAS_OP_REG ? 32 : 8; result.is_signed = 1;
            result.imm_min = -128; result.imm_max = 127; builder.add_operand(result);
        };
        operand(constant, 0, DOMSYMAS_OP_REG, DOMSYMAS_ACC_WRITE); operand(constant, 1, DOMSYMAS_OP_IMM, DOMSYMAS_ACC_READ);
        operand(add, 0, DOMSYMAS_OP_REG, DOMSYMAS_ACC_WRITE); operand(add, 1, DOMSYMAS_OP_REG, DOMSYMAS_ACC_READ); operand(add, 2, DOMSYMAS_OP_REG, DOMSYMAS_ACC_READ);
        operand(ret, 0, DOMSYMAS_OP_REG, DOMSYMAS_ACC_READ);
        builder.add_reloc("ABS32", 32, 0, 0, 0, 0, DOMSYMAS_ADDEND_EXPLICIT, "S + A");
        auto status = builder.build(*bank);
        if (!status.ok()) throw std::runtime_error("test bank build: " + status.message);
        status = bank->validate(true);
        if (!status.ok()) throw std::runtime_error("test bank validation: " + status.message);
    }
    Module source() const {
        auto module = parse(R"(module native level core {func @f()->i32 {^e:
            %a:i32=arith.constant(20:i32); %b:i32=arith.constant(22:i32);
            %r:i32=arith.add(%a,%b); func.return(%r);}})");
        module.target = sandstone_target(*bank, 0, 64); module.target.features.insert("addition"); return module;
    }
    SelectedFunction select() const {
        SelectedFunction result;
        result.scheduling.name = "f"; result.scheduling.entry = 0; result.scheduling.blocks = {{0, "entry", {}, {}}};
        for (unsigned n = 0; n < 4; ++n) {
            auto instruction = sandstone_instruction(*bank, n < 2 ? constant : n == 2 ? add : ret, model, 100 + n);
            if (n < 2) instruction.defs = {n};
            else if (n == 2) { instruction.defs = {2}; instruction.uses = {0, 1}; }
            else { instruction.uses = {2}; instruction.terminator = true; instruction.control = limestone::schedrow::ControlFlow::Return; }
            result.scheduling.instructions.push_back(instruction);
        }
        result.registers.classes = {{"GPR", {0, 1, 2}}};
        result.registers.values = {{0, "GPR", {}, true}, {1, "GPR", {}, true}, {2, "GPR", {}, true}};
        limestone::regtl::Block block{}; block.id = 0;
        for (const auto& s : result.scheduling.instructions) {
            limestone::regtl::Instruction i{}; i.id = s.id; i.defs = s.defs; i.uses = s.uses; block.instructions.push_back(i);
        }
        result.registers.blocks.push_back(block);
        auto value = [](const char* name, unsigned position, unsigned id) {
            FormBinding binding; binding.name = name; binding.position = position; binding.kind = FormBinding::Kind::Value; binding.id = id; return binding;
        };
        auto immediate = [](unsigned number) {
            FormBinding binding; binding.name = "imm"; binding.position = 1; binding.literal.num = number; return binding;
        };
        result.encodings[100] = {constant, {value("dst", 0, 0), immediate(20)}};
        result.encodings[101] = {constant, {value("dst", 0, 1), immediate(22)}};
        result.encodings[102] = {add, {value("dst", 0, 2), value("lhs", 1, 0), value("rhs", 2, 1)}};
        result.encodings[103] = {ret, {value("src", 0, 2)}};
        return result;
    }
    NativeTarget target() const {
        NativeTarget target; target.bank = bank; target.machine = sandstone_machine(*bank, model);
        auto selected = select(); target.select = [selected](const auto&, const auto&, const auto&) { return selected; }; return target;
    }
};
void native_pipeline() {
    Registry registry; Fixture fixture; auto module = fixture.source(); auto target = fixture.target();
    auto before = print(module);
    auto result = compile_native(module, registry, target);
    CHECK(result.artifact.bytes.size() == 12 && result.functions.size() == 1 && result.artifact.symbols.size() == 1);
    CHECK(result.functions[0].selected.instructions.size() == 4 && result.functions[0].encodings.size() == 4);
    CHECK(result.artifact.symbols[0].name == "f" && result.artifact.symbols[0].size == 12);
    const auto& allocation = result.functions[0].allocation;
    CHECK(allocation.spilled.empty() && allocation.regs.size() == 3 && allocation.regs.at(0) != allocation.regs.at(1));
    auto a = static_cast<std::uint8_t>(allocation.regs.at(0)), b = static_cast<std::uint8_t>(allocation.regs.at(1)), out = static_cast<std::uint8_t>(allocation.regs.at(2));
    CHECK(result.artifact.bytes == (std::vector<std::uint8_t>{0x10,a,20,0x10,b,22,0x20,out,a,b,0x30,out}));
    CHECK(result.functions[0].schedule[2].cycle >= 2 && result.functions[0].schedule[3].cycle >= 3);
    CHECK(print(module) == before);
    register_native_backend(registry, "test-native", target);
    CHECK(emit(module, registry, "test-native").bytes == result.artifact.bytes);
    for (auto allocator : {Allocator::LinearScan, Allocator::Greedy, Allocator::Coloring}) {
        target.allocator = allocator; CHECK(compile_native(module, registry, target).artifact.bytes.size() == 12);
    }
    rejects([&] { register_native_backend(registry, "test-native", target); }, "duplicate");
    auto wrong = module; wrong.target.pointer_bits = 32;
    rejects([&] { compile_native(wrong, registry, target); }, "disagree");
    wrong = module; wrong.target.features.erase("addition");
    rejects([&] { compile_native(wrong, registry, target); }, "disabled feature");
    wrong = module; wrong.target.features.insert("unknown");
    rejects([&] { compile_native(wrong, registry, target); }, "unknown requested");
    auto bad = target; bad.machine.issue_width = 0;
    rejects([&] { compile_native(module, registry, bad); }, "issue width");
    auto reject_selection = [&](auto mutation, const char* expected) {
        auto selected = fixture.select(); mutation(selected);
        auto invalid = fixture.target(); invalid.select = [selected](const auto&, const auto&, const auto&) { return selected; };
        rejects([&] { compile_native(module, registry, invalid); }, expected); CHECK(print(module) == before);
    };
    reject_selection([](auto& s) { s.registers.blocks[0].instructions[2].uses.clear(); }, "dataflow");
    reject_selection([](auto& s) { s.encodings.erase(100); }, "encoding map");
    reject_selection([](auto& s) { s.encodings[100].form = 999; }, "unknown selected form");
    reject_selection([](auto& s) { s.registers.classes[0].members = {0}; }, "spill");
    reject_selection([](auto& s) { s.registers.values[0].klass = "UNKNOWN"; }, "register");
    reject_selection([](auto& s) { s.scheduling.deps.push_back({100,100,limestone::schedrow::DepKind::True,0,0,false,{}}); }, "self-dependent");
    reject_selection([](auto& s) { s.encodings[100].bindings[1].literal.num = 1000; }, "constraint");
    reject_selection([](auto& s) { s.encodings[100].bindings.pop_back(); }, "missing visible");
    reject_selection([](auto& s) { s.encodings[100].bindings[0].id = 999; }, "undeclared virtual");
    reject_selection([](auto& s) { s.registers.blocks[0].successors = {1}; s.scheduling.blocks[0].successors = {}; }, "block contracts");
    auto no_timing = *fixture.bank; no_timing.mutable_data().scheds.clear();
    rejects([&] { sandstone_instruction(no_timing, fixture.add, fixture.model, 0); }, "concrete scheduling");
    rejects([&] { sandstone_instruction(*fixture.bank, fixture.add, 999, 0); }, "unknown SymAS scheduling model");
    fixture.bank->mutable_data().registers[0].flags = DOMSYMAS_REG_READABLE;
    reject_selection([](auto& s) { s.registers.values[0].constraint.fixed = 0; }, "constraint");
    fixture.bank->mutable_data().registers[0].flags = DOMSYMAS_REG_READABLE | DOMSYMAS_REG_WRITABLE;
}
void relocation_and_aliases() {
    Registry registry; Fixture fixture;
    auto& bank = fixture.bank->mutable_data();
    const auto* ret = fixture.bank->find_form(fixture.ret); CHECK(ret);
    for (auto& encoder : bank.encoders) if (encoder.id == ret->encoder)
        encoder.source = "emit8(0x30); emit8(src); emit_relocation(\"external\", ABS32); emit32le(0);";
    auto module = fixture.source(); auto other = module.functions[0]; other.name = "g"; module.functions.push_back(other);
    auto result = compile_native(module, registry, fixture.target());
    CHECK(result.artifact.bytes.size() == 32 && result.artifact.relocations.size() == 2);
    CHECK(result.artifact.relocations[0].offset == 12 && result.artifact.relocations[1].offset == 28);
    CHECK(result.artifact.relocations[0].symbol == "external" && result.artifact.relocations[0].kind == "ABS32");
    CHECK(result.artifact.symbols[1].offset == 16);
    bank.relocs[0].offset = 7;
    rejects([&] { compile_native(module, registry, fixture.target()); }, "relocation exceeds");
    bank.relocs[0].offset = 0;
    // Overlapping bank register views must not silently become independent colors.
    domsymas::RegisterRec alias; alias.id = 3; alias.name = "r0_alias"; alias.width = 32; alias.parent = 0; alias.sub_hi = 32;
    alias.flags = DOMSYMAS_REG_READABLE | DOMSYMAS_REG_WRITABLE; bank.registers.push_back(alias);
    auto selected = fixture.select(); selected.registers.classes[0].members = {0,3};
    auto target = fixture.target(); target.select = [selected](const auto&, const auto&, const auto&) { return selected; };
    rejects([&] { compile_native(module, registry, target); }, "spill");
}
#if defined(__clang__)
// Generated machine code has no Clang function-type sanitizer metadata.
__attribute__((no_sanitize("function")))
#endif
void shipped_tiny_targets() {
    Registry registry;
    const auto arches = all_architectures();
    CHECK(arches.size() == 27);
    {
        std::set<std::string> keys;
        for (const auto& info : arches) {
            CHECK(!info.key.empty() && !info.arch.empty() && !info.abi.empty());
            CHECK(info.addr_size == 16 || info.addr_size == 32 || info.addr_size == 64);
            CHECK(info.endian == "little" || info.endian == "big");
            CHECK(keys.insert(info.key).second);
        }
    }
    const auto* aarch64_info = find_architecture("aarch64");
    CHECK(aarch64_info && aarch64_info->addr_size == 64 && aarch64_info->endian == "little" && aarch64_info->abi == "AAPCS64");
    CHECK(find_architecture("no-such-arch") == nullptr);
    CHECK(tiny_backend_names() == (std::vector<std::string>{"x86-64-tiny", "aarch64-tiny", "riscv64-tiny"}));
    CHECK(tiny_arch_name(TinyArch::X86_64) == "x86-64-tiny");
    auto add_module = parse(R"(module tiny level core { func @f() -> i32 {
      ^e: %a:i32 = arith.constant(20:i32); %b:i32 = arith.constant(22:i32);
      %r:i32 = arith.add(%a, %b); func.return(%r); } })");
    auto submul_module = parse(R"(module tiny level core { func @g() -> i32 {
      ^e: %x:i32 = arith.constant(100:i32); %y:i32 = arith.constant(7:i32); %z:i32 = arith.constant(3:i32);
      %d:i32 = arith.sub(%x, %y); %m:i32 = arith.mul(%d, %z); %c:i32 = arith.copy(%m); func.return(%c); } })");
    auto emit32 = [](std::vector<std::uint8_t>& out, std::uint32_t word) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(word >> (i * 8)));
    };
    auto modrm = [](std::uint32_t reg, std::uint32_t rm) { return static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7)); };
    const std::pair<TinyArch, const char*> targets[] = {
        {TinyArch::X86_64, "x86-64-tiny"}, {TinyArch::AArch64, "aarch64-tiny"}, {TinyArch::RiscV64, "riscv64-tiny"},
    };
    for (const auto& [arch, name] : targets) {
        auto target = make_tiny_target(arch);
        auto enc = [&](std::uint32_t preg) {
            for (const auto& record : target.bank->data().registers)
                if (record.id == preg) return record.enc_num;
            throw std::runtime_error("unknown physical register");
        };
        auto compile = [&](const Module& source) {
            auto module = source;
            module.target = sandstone_target(*target.bank, 0, 64);
            return compile_native(module, registry, target);
        };
        for (const auto* function : {"f", "g"}) {
            const auto& source = std::string(function) == "f" ? add_module : submul_module;
            auto result = compile(source);
            CHECK(result.functions.size() == 1 && result.artifact.relocations.empty());
            CHECK(result.artifact.symbols.size() == 1 && result.artifact.symbols[0].name == function);
            const auto& allocation = result.functions[0].allocation;
            CHECK(allocation.spilled.empty());
            // Independent bit-level expectations from the allocated registers.
            std::vector<std::uint8_t> expected;
            auto li = [&](std::uint32_t value, std::int64_t imm) {
                const auto e = enc(allocation.regs.at(value));
                if (arch == TinyArch::X86_64) {
                    expected.push_back(static_cast<std::uint8_t>(0xB8 + e));
                    auto u = static_cast<std::uint32_t>(imm);
                    for (int i = 0; i < 4; ++i) expected.push_back(static_cast<std::uint8_t>(u >> (i * 8)));
                } else if (arch == TinyArch::AArch64) {
                    emit32(expected, 0x52800000u | (static_cast<std::uint32_t>(imm) << 5) | e);
                } else {
                    const auto u = static_cast<std::uint32_t>(imm & 0xFFF);
                    emit32(expected, (u << 20) | (e << 7) | 0x13u);
                }
            };
            auto alu = [&](std::uint32_t base, std::uint32_t dst, std::uint32_t lhs, std::uint32_t rhs) {
                const auto d = enc(allocation.regs.at(dst)), l = enc(allocation.regs.at(lhs)), r = enc(allocation.regs.at(rhs));
                if (arch == TinyArch::X86_64) {
                    // Two-address expansion: mov dst,lhs then the operation.
                    expected.push_back(0x89);
                    expected.push_back(modrm(l, d));
                    if (base == 0) expected.push_back(0x01);
                    else if (base == 1) expected.push_back(0x29);
                    else {
                        expected.push_back(0x0F);
                        expected.push_back(0xAF);
                    }
                    expected.push_back(modrm(r, d));
                } else if (arch == TinyArch::AArch64) {
                    emit32(expected, base | (r << 16) | (l << 5) | d);
                } else {
                    emit32(expected, base | (r << 20) | (l << 15) | (d << 7));
                }
            };
            auto ret = [&] {
                if (arch == TinyArch::X86_64) expected.push_back(0xC3);
                else if (arch == TinyArch::AArch64) emit32(expected, 0xD65F03C0u);
                else emit32(expected, 0x00008067u);
            };
            const auto& fn = source.function(function);
            for (const auto& block : fn.blocks)
                for (const auto& op : block.operations) {
                    if (op.name == "arith.constant") li(op.results[0].id, op.operands[0].integer);
                    if (op.name == "arith.add" || op.name == "arith.sub" || op.name == "arith.mul") {
                        std::uint32_t base = 0;
                        if (arch == TinyArch::X86_64) base = op.name == "arith.add" ? 0 : op.name == "arith.sub" ? 1 : 2;
                        else if (arch == TinyArch::AArch64)
                            base = op.name == "arith.add" ? 0x0B000000u : op.name == "arith.sub" ? 0x4B000000u : 0x1B007C00u;
                        else base = op.name == "arith.add" ? 0x33u : op.name == "arith.sub" ? 0x40000033u : 0x02000033u;
                        alu(base, op.results[0].id, op.operands[0].value, op.operands[1].value);
                    } else if (op.name == "arith.copy") {
                        const auto d = enc(allocation.regs.at(op.results[0].id)), s = enc(allocation.regs.at(op.operands[0].value));
                        if (arch == TinyArch::X86_64) {
                            expected.push_back(0x89);
                            expected.push_back(modrm(s, d));
                        } else if (arch == TinyArch::AArch64) {
                            emit32(expected, 0x0B000000u | (31u << 16) | (s << 5) | d);
                        } else {
                            emit32(expected, (s << 15) | (d << 7) | 0x13u);
                        }
                    }
                }
            ret();
            CHECK(result.artifact.bytes == expected);
        }
        // Rejections: multi-block, calls, wide types, void returns, immediates.
        auto sum = example("sum.flint");
        sum.target = sandstone_target(*target.bank, 0, 64);
        try {
            compile_native(sum, registry, target);
            throw std::runtime_error("expected tiny.cfg rejection");
        } catch (const std::exception& e) {
            CHECK(std::string(e.what()).find("tiny.cfg") != std::string::npos);
        }
        auto wide = parse("module m level core {func @f()->i64 {^e:%r:i64=arith.add(1:i64,2:i64);func.return(%r);}}");
        wide.target = sandstone_target(*target.bank, 0, 64);
        try {
            compile_native(wide, registry, target);
            throw std::runtime_error("expected tiny.type rejection");
        } catch (const std::exception& e) {
            CHECK(std::string(e.what()).find("tiny.type") != std::string::npos);
        }
        auto called = parse("module m level core {extern func @g()->i32; func @f()->i32 {^e:%r:i32=func.call() attrs {callee=\"g\";};func.return(%r);}}");
        called.target = sandstone_target(*target.bank, 0, 64);
        try {
            compile_native(called, registry, target);
            throw std::runtime_error("expected tiny.unsupported rejection");
        } catch (const std::exception& e) {
            CHECK(std::string(e.what()).find("tiny.unsupported") != std::string::npos);
        }
        auto empty = parse("module m level core {func @f()->void {^e:func.return();}}");
        empty.target = sandstone_target(*target.bank, 0, 64);
        try {
            compile_native(empty, registry, target);
            throw std::runtime_error("expected void rejection");
        } catch (const std::exception& e) {
            CHECK(std::string(e.what()).find("missing visible") != std::string::npos);
        }
        // PBQP must also solve the same problem.
        auto pbqp = target;
        pbqp.allocator = Allocator::PBQP;
        auto pbqp_module = add_module;
        pbqp_module.target = sandstone_target(*target.bank, 0, 64);
        auto via_pbqp = compile_native(pbqp_module, registry, pbqp);
        CHECK(via_pbqp.functions[0].allocation.spilled.empty());
        CHECK(!via_pbqp.artifact.bytes.empty());
        // Backend registration by name, idempotent.
        register_tiny_backends(registry);
        register_tiny_backends(registry);
        auto named = add_module;
        named.target = sandstone_target(*target.bank, 0, 64);
        CHECK(emit(named, registry, name).bytes == compile(named).artifact.bytes);
#if defined(__linux__) && defined(__x86_64__)
        if (arch == TinyArch::X86_64) {
            auto hosted = compile(add_module);
            const auto page = sysconf(_SC_PAGESIZE);
            CHECK(page > 0);
            void* memory = mmap(nullptr, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            CHECK(memory != MAP_FAILED);
            std::memcpy(memory, hosted.artifact.bytes.data(), hosted.artifact.bytes.size());
            CHECK(mprotect(memory, static_cast<std::size_t>(page), PROT_READ | PROT_EXEC) == 0);
            const auto answer = reinterpret_cast<int (*)()>(memory)();
            CHECK(munmap(memory, static_cast<std::size_t>(page)) == 0);
            CHECK(answer == 42);
        }
#endif
    }
}
#if defined(__clang__)
// Generated machine code has no Clang function-type sanitizer metadata.
__attribute__((no_sanitize("function")))
#endif
void executable_native_subset() {
#if defined(__linux__) && defined(__x86_64__)
    // An explicit, deliberately tiny AMD64 target. Scheduling costs remain
    // synthetic test policy, not claimed microarchitectural facts.
    Fixture fixture; Registry registry;
    auto& data = fixture.bank->mutable_data(); data.targets[0].arch = "x86-64";
    for (auto& encoder : data.encoders) {
        if (encoder.id == fixture.bank->find_form(fixture.constant)->encoder) encoder.source = "emit8(0xB8 + dst); emit32le(imm);";
        if (encoder.id == fixture.bank->find_form(fixture.add)->encoder) encoder.source = "require(dst == lhs); emit8(0x01); emit_modrm(rhs, dst);";
        if (encoder.id == fixture.bank->find_form(fixture.ret)->encoder) encoder.source = "emit8(0xC3);";
    }
    for (auto& form : data.forms) if (form.id == fixture.ret) form.operand_count = 0;
    std::erase_if(data.operands, [&](const auto& operand) { return operand.form == fixture.ret; });
    auto selected = fixture.select(); selected.encodings[103].bindings.clear();
    selected.scheduling.instructions[2].ties = {{2,0}};
    selected.registers.blocks[0].instructions[2].ties = {{2,0}};
    selected.registers.values[2].constraint.fixed = 0; // EAX: SysV i32 return.
    auto target = fixture.target(); target.select = [selected](const auto&, const auto&, const auto&) { return selected; };
    auto result = compile_native(fixture.source(), registry, target);
    CHECK(result.artifact.relocations.empty());
    CHECK(result.artifact.bytes.size() == 13 && result.artifact.bytes.back() == 0xC3);
    const auto page = sysconf(_SC_PAGESIZE); CHECK(page > 0);
    void* memory = mmap(nullptr, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);
    std::memcpy(memory, result.artifact.bytes.data(), result.artifact.bytes.size());
    if (mprotect(memory, static_cast<std::size_t>(page), PROT_READ | PROT_EXEC) != 0) {
        munmap(memory, static_cast<std::size_t>(page)); throw std::runtime_error("cannot make the test's generated code executable");
    }
    auto function = reinterpret_cast<int(*)()>(memory);
    auto answer = function();
    CHECK(munmap(memory, static_cast<std::size_t>(page)) == 0);
    CHECK(answer == 42);
#endif
}
}
int main() {
    try {
        native_pipeline();
        relocation_and_aliases();
        executable_native_subset();
        shipped_tiny_targets();
        std::cout << "Stone native handoffs passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
