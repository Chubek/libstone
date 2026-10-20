#include "Native.hpp"
#include "../internal.hpp"
#include <limits>

namespace flintstone {
namespace {
template<class T> T unwrap(limestone::Result<T> result, const char* stage) {
    if (!result) detail::fail(std::string("native.") + stage, result.error().message);
    return std::move(result.value());
}
const domsymas::TargetRec& target_record(const domsymas::Databank& bank, std::uint32_t id) {
    for (const auto& t : bank.data().targets) if (t.id == id) return t;
    detail::fail("sandstone.target", "unknown SymAS target record");
}
const domsymas::RegisterRec& register_record(const domsymas::Databank& bank, std::uint32_t id) {
    for (const auto& r : bank.data().registers) if (r.id == id) return r;
    detail::fail("sandstone.register", "unknown SymAS physical register");
}
template<class T> std::set<T> members(const std::vector<T>& list) { return {list.begin(), list.end()}; }
std::vector<std::pair<std::uint32_t, std::uint32_t>> aliases(const domsymas::Databank& bank, const limestone::regtl::Function& registers) {
    auto result = registers.aliases;
    struct Slice { std::uint32_t root; std::uint64_t begin, end; };
    auto slice = [&](std::uint32_t id) {
        const auto& first = register_record(bank, id);
        Slice range{id, 0, first.width}; std::set<std::uint32_t> seen;
        for (;;) {
            detail::check(seen.insert(id).second, "sandstone.register", "cyclic register parents");
            const auto& r = register_record(bank, id);
            if (r.parent == domsymas::kNoParent) { range.root = id; return range; }
            const auto& parent = register_record(bank, r.parent);
            detail::check(std::uint64_t(r.sub_lo) + r.width <= parent.width, "sandstone.register", "register view exceeds its parent storage");
            range.begin += r.sub_lo; range.end += r.sub_lo; id = r.parent;
        }
    };
    std::set<std::uint32_t> ids;
    for (const auto& c : registers.classes) ids.insert(c.members.begin(), c.members.end());
    std::map<std::uint32_t, Slice> slices;
    for (auto id : ids) slices.emplace(id, slice(id));
    for (auto a = ids.begin(); a != ids.end(); ++a) for (auto b = std::next(a); b != ids.end(); ++b) {
        auto x = slices.at(*a), y = slices.at(*b);
        if (x.root == y.root && x.begin < y.end && y.begin < x.end) result.emplace_back(*a, *b);
    }
    std::sort(result.begin(), result.end()); result.erase(std::unique(result.begin(), result.end()), result.end()); return result;
}
}
Target sandstone_target(const domsymas::Databank& bank, std::uint32_t id, unsigned index_bits) {
    const auto& record = target_record(bank, id);
    detail::check((record.addr_size == 8 || record.addr_size == 16 || record.addr_size == 32 || record.addr_size == 64) &&
                      record.byte_order <= 1,
                  "sandstone.layout", "SymAS target lacks a supported data layout");
    detail::check(index_bits >= 2 && index_bits <= 64, "sandstone.layout", "index width must be in 2..64");
    Target result; result.name = record.arch; result.pointer_bits = record.addr_size; result.index_bits = index_bits;
    result.endian = record.byte_order == 0 ? Endian::Little : Endian::Big;
    for (auto feature : record.features) {
        auto found = std::find_if(bank.data().features.begin(), bank.data().features.end(), [&](const auto& f) { return f.id == feature; });
        detail::check(found != bank.data().features.end(), "sandstone.feature", "target has an unknown feature"); result.features.insert(found->name);
    }
    return result;
}
limestone::schedrow::MachineModel sandstone_machine(const domsymas::Databank& bank, std::uint32_t id) {
    auto model = std::find_if(bank.data().models.begin(), bank.data().models.end(), [&](const auto& m) { return m.id == id; });
    detail::check(model != bank.data().models.end() && model->issue_width, "sandstone.model", "model has no explicit issue width");
    limestone::schedrow::MachineModel result; result.issue_width = model->issue_width;
    for (const auto* unit : bank.units_for_model(id)) {
        detail::check(unit->count && !unit->name.empty() && result.resource_capacity.emplace(unit->name, unit->count).second,
                      "sandstone.unit", "invalid or duplicate model resource capacity");
    }
    return result;
}
limestone::schedrow::Instruction sandstone_instruction(const domsymas::Databank& bank, std::uint32_t form, std::uint32_t model, std::uint32_t id) {
    const auto* record = bank.find_form(form);
    detail::check(record, "sandstone.form", "unknown SymAS form");
    detail::check(model == domsymas::kNoId || std::any_of(bank.data().models.begin(), bank.data().models.end(), [&](const auto& m) { return m.id == model; }),
                  "sandstone.model", "unknown SymAS scheduling model");
    auto timing = bank.schedule_for_form(form, model);
    detail::check(timing.found && timing.latency != domsymas::kVariadicLatency && timing.cycles && timing.uops,
                  "sandstone.timing", "instruction lacks a concrete scheduling contract");
    limestone::schedrow::Instruction result{};
    result.id = id; result.latency = timing.latency; result.throughput = double(timing.cycles) / timing.uops;
    result.opcode = "symas.form." + std::to_string(form); result.origin = "SymAS form " + std::to_string(form);
    auto units = timing.writes; units.insert(units.end(), timing.reads.begin(), timing.reads.end());
    std::sort(units.begin(), units.end()); units.erase(std::unique(units.begin(), units.end()), units.end());
    for (auto id : units) {
        auto found = std::find_if(bank.data().units.begin(), bank.data().units.end(), [&](const auto& u) { return u.id == id; });
        detail::check(found != bank.data().units.end() && (found->model == domsymas::kNoId || found->model == model), "sandstone.unit", "unknown or inapplicable scheduling unit");
        result.resources.push_back({found->name, timing.cycles, 1, 0, {}});
    }
    // Semantic effects/CFG/implicit registers are supplied by the selector, not
    // guessed from a mnemonic or from a timing class.
    return result;
}
NativeCompilation compile_native(const Module& source, const Registry& registry, const NativeTarget& target) {
    detail::check(target.bank && target.select, "native.target", "native target needs a bank and an explicit selector");
    const auto& bank = *target.bank;
    auto valid = bank.validate(true);
    detail::check(valid.ok(), "sandstone.validation", valid.message);
    auto module = instantiate(source, registry);
    const auto expected = sandstone_target(bank, target.target_id, module.target.index_bits);
    detail::check(module.target.name == expected.name && module.target.pointer_bits == expected.pointer_bits &&
                      module.target.index_bits == expected.index_bits && module.target.endian == expected.endian,
                  "native.target", "module and SymAS target layouts/architectures disagree");
    detail::check(target.machine.issue_width > 0, "native.timing", "an explicit machine issue width is required");
    std::set<std::uint32_t> features;
    for (const auto& name : expected.features) detail::check(module.target.features.count(name), "native.feature", "module omits a mandatory target feature");
    for (const auto& name : module.target.features) {
        const auto* feature = bank.find_feature(name);
        detail::check(feature, "native.feature", "unknown requested SymAS feature " + name); features.insert(feature->id);
    }
    NativeCompilation result; result.artifact.format = "stone-native-relocatable";
    for (const auto& function : module.functions) if (!function.external) {
        auto selected = target.select(module, function, registry);
        auto& region = selected.scheduling; auto& registers = selected.registers;
        detail::check(!region.blocks.empty() && region.entry == region.blocks.front().id, "native.cfg", "selector must supply an explicit entry-first block layout");
        std::map<std::uint32_t, const limestone::schedrow::Instruction*> instructions;
        for (const auto& instruction : region.instructions) instructions[instruction.id] = &instruction;
        detail::check(instructions.size() == region.instructions.size() && selected.encodings.size() == instructions.size(), "native.identity", "duplicate instructions or incomplete encoding map");
        for (const auto& instruction : region.instructions) {
            detail::check(selected.encodings.count(instruction.id), "native.encoding", "missing selected form");
            const auto* form = bank.find_form(selected.encodings.at(instruction.id).form);
            detail::check(form, "native.form", "unknown selected form");
            const auto flags = form->flags;
            detail::check(!(flags & DOMSYMAS_IF_MAY_TRAP) || instruction.may_trap, "native.effects", "selector omitted a form's trapping effect");
            detail::check(!(flags & DOMSYMAS_IF_READS_MEM) || (instruction.access && instruction.access->read), "native.effects", "selector omitted a form's memory read");
            detail::check(!(flags & DOMSYMAS_IF_WRITES_MEM) || (instruction.access && instruction.access->write), "native.effects", "selector omitted a form's memory write");
            detail::check(!(flags & DOMSYMAS_IF_TERMINATOR) || instruction.terminator, "native.effects", "selector omitted a form's terminator effect");
            detail::check(!(flags & DOMSYMAS_IF_RETURN) || instruction.control == limestone::schedrow::ControlFlow::Return, "native.effects", "selector omitted a form's return effect");
            detail::check(!(flags & DOMSYMAS_IF_BRANCH) || instruction.control == limestone::schedrow::ControlFlow::Branch || instruction.control == limestone::schedrow::ControlFlow::ConditionalBranch || instruction.control == limestone::schedrow::ControlFlow::IndirectBranch,
                          "native.effects", "selector omitted a form's branch effect");
            detail::check(!(flags & DOMSYMAS_IF_COND_BRANCH) || instruction.control == limestone::schedrow::ControlFlow::ConditionalBranch,
                          "native.effects", "selector omitted a form's conditional branch effect");
            detail::check(!(flags & DOMSYMAS_IF_DELAYED), "native.effects", "delay slots require an explicit materialization adapter");
        }
        registers.aliases = aliases(bank, registers);
        auto machine = target.machine; machine.register_aliases = registers.aliases;
        std::map<std::uint32_t, limestone::regtl::Instruction> transfers;
        std::map<std::uint32_t, limestone::regtl::Block> blocks;
        for (const auto& b : registers.blocks) {
            detail::check(blocks.emplace(b.id, b).second, "native.cfg", "duplicate allocation block");
            for (const auto& i : b.instructions) {
                detail::check(transfers.emplace(i.id, i).second && instructions.count(i.id), "native.identity", "duplicate or extra allocation instruction");
                const auto& s = *instructions.at(i.id);
                auto implicit_defs = members(i.physical_defs); implicit_defs.insert(i.clobbers.begin(), i.clobbers.end());
                detail::check(s.block == b.id && members(s.defs) == members(i.defs) && members(s.uses) == members(i.uses) &&
                              members(s.early_defs) == members(i.early_defs) && members(s.ties) == members(i.ties) &&
                              implicit_defs == members(s.implicit_defs) && members(s.implicit_uses) == members(i.physical_uses),
                              "native.dataflow", "scheduling/allocation instruction contracts disagree");
            }
        }
        detail::check(transfers.size() == instructions.size() && blocks.size() == region.blocks.size(), "native.identity", "incomplete allocation CFG");
        for (const auto& value : registers.values) {
            const limestone::regtl::RegClass* klass = nullptr;
            for (const auto& candidate : registers.classes) if (candidate.name == value.klass) klass = &candidate;
            detail::check(klass != nullptr && !klass->members.empty(), "native.register",
                          "unknown or empty register class '" + value.klass + "'");
            for (auto reg : klass->members) register_record(bank, reg);
        }
        auto schedule = unwrap(limestone::schedrow::schedule_cfg(region, machine), "schedule");
        unwrap(limestone::schedrow::verify_cfg(region, machine, schedule), "schedule-verification");
        registers.blocks.clear();
        for (const auto& b : region.blocks) {
            auto found = blocks.find(b.id);
            detail::check(found != blocks.end(), "native.cfg", "allocation is missing scheduling block");
            // Successor order matters for (cond, yes, no) branches; compare ordered vectors, not sets.
            detail::check(found->second.successors == b.successors && members(found->second.live_out) == members(b.live_out),
                          "native.cfg", "scheduling/allocation block contracts disagree");
            auto allocation_block = blocks.at(b.id); allocation_block.instructions.clear();
            for (const auto& scheduled : schedule) if (instructions.at(scheduled.id)->block == b.id) allocation_block.instructions.push_back(transfers.at(scheduled.id));
            registers.blocks.push_back(std::move(allocation_block));
        }
        auto analysis = unwrap(limestone::regtl::analyze(registers), "liveness");
        limestone::regtl::Allocation allocation;
        switch (target.allocator) {
        case Allocator::LinearScan: allocation = unwrap(limestone::regtl::linear_scan(analysis.problem), "allocation"); break;
        case Allocator::Greedy: allocation = unwrap(limestone::regtl::greedy(analysis.problem), "allocation"); break;
        case Allocator::Coloring: allocation = unwrap(limestone::regtl::graph_color(analysis.problem), "allocation"); break;
        case Allocator::PBQP: allocation = unwrap(limestone::regtl::pbqp_allocate(analysis.problem), "allocation"); break;
        default: detail::fail("native.allocator", "unknown allocator");
        }
        unwrap(limestone::regtl::verify(analysis.problem, allocation), "allocation-verification");
        detail::check(allocation.spilled.empty(), "native.spill", "allocation requires an explicit spill/reload materialization adapter");
        const auto instruction_alignment = target_record(bank, target.target_id).instr_align;
        detail::check(instruction_alignment && !(instruction_alignment & (instruction_alignment - 1)), "native.alignment", "invalid target instruction alignment");
        const auto start = result.artifact.bytes.size();
        for (const auto& entry : schedule) {
            detail::check(selected.encodings.count(entry.id), "native.encoding", "missing selected form");
            const auto& encoding = selected.encodings.at(entry.id);
            const auto* form = bank.find_form(encoding.form);
            detail::check(form, "native.form", "unknown selected form");
            for (auto feature : form->required) detail::check(features.count(feature), "native.feature", "selected form requires a disabled feature");
            for (auto feature : form->forbidden) detail::check(!features.count(feature), "native.feature", "selected form forbids an enabled feature");
            std::map<std::string, domsymas::encoding::Value> bindings;
            std::set<std::uint32_t> positions;
            const auto form_operands = bank.operands_for_form(form->id);
            for (const auto& binding : encoding.bindings) {
                auto value = binding.literal;
                const domsymas::RegisterRec* physical = nullptr;
                std::uint32_t required_access = 0;
                auto selected_instruction = instructions.find(entry.id);
                detail::check(selected_instruction != instructions.end(), "native.identity", "schedule references an unknown instruction");
                const auto& instruction = *selected_instruction->second;
                if (binding.kind == FormBinding::Kind::Value) {
                    auto known = members(instruction.defs); known.insert(instruction.uses.begin(), instruction.uses.end());
                    detail::check(known.count(binding.id), "native.binding", "encoding references an undeclared virtual operand");
                    auto allocated = allocation.regs.find(binding.id);
                    detail::check(allocated != allocation.regs.end(), "native.binding", "encoding references an unallocated virtual operand");
                    if (members(instruction.defs).count(binding.id)) required_access |= DOMSYMAS_ACC_WRITE;
                    if (members(instruction.uses).count(binding.id)) required_access |= DOMSYMAS_ACC_READ;
                    physical = &register_record(bank, allocated->second);
                } else if (binding.kind == FormBinding::Kind::PhysicalRegister) {
                    auto known = members(instruction.implicit_defs); known.insert(instruction.implicit_uses.begin(), instruction.implicit_uses.end());
                    detail::check(known.count(binding.id), "native.binding", "physical encoder operand lacks a dataflow contract");
                    if (members(instruction.implicit_defs).count(binding.id)) required_access |= DOMSYMAS_ACC_WRITE;
                    if (members(instruction.implicit_uses).count(binding.id)) required_access |= DOMSYMAS_ACC_READ;
                    physical = &register_record(bank, binding.id);
                } else detail::check(binding.kind == FormBinding::Kind::Literal, "native.binding", "invalid encoder binding kind");
                if (physical) { value.is_str = false; value.str.clear(); value.num = physical->enc_num; }
                if (binding.position != domsymas::kNoId) {
                    detail::check(positions.insert(binding.position).second, "native.binding", "duplicate form operand position");
                    auto found = std::find_if(form_operands.begin(), form_operands.end(), [&](const auto* o) { return o->position == binding.position; });
                    detail::check(found != form_operands.end(), "native.binding", "unknown form operand position");
                    const auto& operand = **found;
                    bool basic = false;
                    bool needs_proof = !operand.constraint.empty();
                    auto kind = static_cast<domsymas::OperandKind>(operand.kind);
                    if (kind == domsymas::OperandKind::Reg) {
                        required_access |= operand.access;
                        basic = physical && physical->regclass == operand.regclass && physical->width >= operand.width &&
                            !(required_access & ~DOMSYMAS_ACC_READWRITE) && (physical->flags & required_access) == required_access;
                    } else if (kind == domsymas::OperandKind::Sym) {
                        basic = !physical && value.is_str && !value.str.empty();
                    } else if (kind == domsymas::OperandKind::Imm || kind == domsymas::OperandKind::Disp || kind == domsymas::OperandKind::Abs ||
                               kind == domsymas::OperandKind::Rel || kind == domsymas::OperandKind::Cond) {
                        if (operand.is_signed)
                            basic = !physical && !value.is_str && value.num >= operand.imm_min && value.num <= operand.imm_max;
                        else
                            basic = !physical && !value.is_str && value.num >= 0 &&
                                    static_cast<std::uint64_t>(value.num) >= static_cast<std::uint64_t>(operand.imm_min) &&
                                    static_cast<std::uint64_t>(value.num) <= static_cast<std::uint64_t>(operand.imm_max);
                    } else {
                        detail::fail("native.operand", "unsupported operand kind for form encoding");
                    }
                    // A target proof covers extra target-specific constraints, never a
                    // failed basic legality check (wrong class, width, range, symbol).
                    if (needs_proof) detail::check(basic, "native.operand", "basic operand legality failed; target proof cannot override it");
                    if (needs_proof)
                        detail::check(target.prove_operand && target.prove_operand(operand, value), "native.operand", "operand constraint is unproved or unsupported");
                    else
                        detail::check(basic, "native.operand", "operand constraint is unproved or unsupported");
                } else detail::check(binding.kind == FormBinding::Kind::Literal, "native.binding", "extra encoder metadata must be literal");
                detail::check(!binding.name.empty() && bindings.emplace(binding.name, value).second, "native.binding", "empty or duplicate encoder binding name");
            }
            for (const auto* operand : form_operands) detail::check(operand->hidden || positions.count(operand->position), "native.binding", "missing visible form operand");
            domsymas::Databank::EncodeResult encoded;
            auto status = bank.encode(form->id, bindings, encoded);
            detail::check(status.ok(), "native.encoding", status.message);
            auto base = result.artifact.bytes.size();
            detail::check(base % instruction_alignment == 0, "native.alignment", "target must supply explicit alignment padding");
            detail::check(encoded.bytes.size() <= 64 * 1024 * 1024 - std::min<std::size_t>(base, 64 * 1024 * 1024), "native.limit", "native image size limit exceeded");
            std::vector<std::pair<std::size_t, std::size_t>> claimed;
            for (const auto& relocation : encoded.relocs) {
                auto found = std::find_if(bank.data().relocs.begin(), bank.data().relocs.end(), [&](const auto& r) { return r.id == relocation.type; });
                detail::check(found != bank.data().relocs.end(), "native.relocation", "unknown relocation type");
                auto width = (std::uint64_t(found->width) + found->offset + 7) / 8;
                detail::check(relocation.offset <= encoded.bytes.size() && width <= encoded.bytes.size() - relocation.offset, "native.relocation", "relocation exceeds instruction bytes");
                detail::check(!relocation.symbol.empty(), "native.relocation", "relocation lacks a symbol");
                std::pair<std::size_t, std::size_t> range{relocation.offset, relocation.offset + static_cast<std::size_t>(width)};
                for (const auto& other : claimed)
                    detail::check(range.second <= other.first || other.second <= range.first, "native.relocation", "overlapping relocations");
                claimed.push_back(range);
                result.artifact.relocations.push_back({base + relocation.offset, found->name, relocation.symbol, relocation.addend});
            }
            result.artifact.bytes.insert(result.artifact.bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
        }
        result.artifact.symbols.push_back({function.name, start, result.artifact.bytes.size() - start});
        result.functions.push_back({function.name, std::move(region), std::move(selected.encodings), std::move(schedule), std::move(analysis.problem), std::move(allocation)});
    }
    return result;
}
void register_native_backend(Registry& registry, std::string name, NativeTarget target) {
    registry.add_backend(std::move(name), [target = std::move(target)](const Module& module, const Registry& registry) {
        return compile_native(module, registry, target).artifact;
    });
}
} // namespace flintstone
