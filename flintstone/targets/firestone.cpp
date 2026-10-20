#include "../internal.hpp"
#include <libvm/BytecodeBinary.hpp>
#include <libvm/Compiler.hpp>
#include <libvm/RegisterVM.hpp>
#include <libvm/StackVM.hpp>
#include <bit>
#include <charconv>
#include <cmath>

namespace flintstone {
namespace detail {
firestone::ir::Type firestone_type(const Type& type) {
    using F = firestone::ir::Type;
    switch (type.kind) {
    case TypeKind::Void: return F::void_type();
    case TypeKind::Integer: return F::integer(type.bits);
    case TypeKind::Float: return F::floating(type.bits);
    case TypeKind::Pointer: return F::pointer(type.name);
    default: fail("ssa.type", "type requires explicit scalarization before Firestone lowering: " + print(type));
    }
}
Type from_firestone_type(const firestone::ir::Type& type) {
    using K = firestone::ir::TypeKind;
    switch (type.kind) {
    case K::Void: return Type::void_type();
    case K::Integer: check(type.name.empty(), "ssa.type", "named integers require a type adapter"); return Type::integer(type.bits);
    case K::Float: check(type.name.empty(), "ssa.type", "named floats require a type adapter"); return Type::floating(type.bits);
    case K::Pointer: return Type::pointer(type.name.empty() ? "generic" : type.name);
    default: fail("ssa.type", "Firestone type has no Flintstone scalar representation");
    }
}
namespace {
std::string operation_name(firestone::ir::Opcode code) {
    using O = firestone::ir::Opcode;
    switch (code) {
    case O::Constant: return "arith.constant"; case O::Copy: return "arith.copy";
    case O::Add: return "arith.add"; case O::Sub: return "arith.sub"; case O::Mul: return "arith.mul";
    case O::Div: return "arith.div"; case O::Mod: return "arith.mod"; case O::And: return "arith.and";
    case O::Or: return "arith.or"; case O::Xor: return "arith.xor"; case O::Shl: return "arith.shl"; case O::Shr: return "arith.shr";
    case O::Neg: return "arith.neg"; case O::Not: return "arith.not";
    case O::Eq: return "arith.eq"; case O::Ne: return "arith.ne"; case O::Lt: return "arith.lt";
    case O::Le: return "arith.le"; case O::Gt: return "arith.gt"; case O::Ge: return "arith.ge";
    case O::Phi: return "cf.phi"; case O::Select: return "arith.select";
    case O::Load: return "mem.load"; case O::Store: return "mem.store";
    case O::Call: return "func.call"; case O::Jump: return "cf.jump";
    case O::Branch: return "cf.branch"; case O::Return: return "func.return";
    }
    fail("ssa.opcode", "unknown Firestone opcode");
}
}
Function from_ssa(const firestone::ir::SimpleSSAGraph& graph, const Function& original) {
    check(graph.verify().empty(), "ssa.invalid", "cannot import invalid SSA");
    Function result = original; result.blocks.clear(); result.parameters.clear();
    std::map<firestone::ir::ValueId, ValueId> values;
    std::map<ValueId, const Operation*> provenance;
    firestone::ir::ValueId next = 0;
    for (const auto& p : original.parameters) values[next++] = p.id;
    bool any_vm = false, any_core = false;
    for (const auto& b : original.blocks) for (const auto& op : b.operations) {
        if (op.name.rfind("vm.", 0) == 0) any_vm = true; else any_core = true;
        for (const auto& r : op.results) { values[next++] = r.id; provenance[r.id] = &op; }
    }
    // Mixed core/vm levels would silently promote every op; require one level here.
    check(!(any_vm && any_core), "ssa.level", "mixed core/vm operations require explicit lowering before SSA import");
    auto fresh = original.fresh_value();
    for (firestone::ir::ValueId n = next; n < graph.value_count(); ++n) {
        check(fresh < invalid_id - 1, "id.exhausted", "SSA import exhausted value IDs"); values[n] = fresh++;
    }
    auto definitions = graph.definitions();
    for (const auto& [id, definition] : definitions) if (definition.parameter) {
        auto mapped = values.find(id);
        check(mapped != values.end(), "ssa.parameter", "optimizer changed function parameters");
        auto old = std::find_if(original.parameters.begin(), original.parameters.end(), [&](const Value& v) { return v.id == mapped->second; });
        check(old != original.parameters.end() && old->type == from_firestone_type(graph.type(id)), "ssa.parameter", "optimizer changed parameter signature");
        result.parameters.push_back(*old);
    }
    std::map<firestone::ir::BlockId, BlockId> blocks;
    auto graph_blocks = graph.cfg().blocks();
    check(graph_blocks.size() == original.blocks.size(), "ssa.cfg", "optimizer changed CFG block identities");
    for (std::size_t n = 0; n < graph_blocks.size(); ++n) {
        const auto& graph_name = graph.cfg().block(graph_blocks[n]).name;
        const auto& original_name = original.blocks[n].name;
        check(graph_name == original_name, "ssa.cfg", "optimizer changed CFG block order or names");
        blocks[graph_blocks[n]] = original.blocks[n].id;
    }
    {
        auto entry = blocks.find(graph.cfg().entry());
        check(entry != blocks.end(), "ssa.cfg", "optimizer changed CFG entry");
        result.entry = entry->second;
    }
    auto lookup_value = [&](firestone::ir::ValueId id) {
        auto found = values.find(id);
        check(found != values.end(), "ssa.value", "SSA value has no Flintstone mapping");
        return found->second;
    };
    auto lookup_block = [&](firestone::ir::BlockId id) {
        auto found = blocks.find(id);
        check(found != blocks.end(), "ssa.cfg", "SSA block has no Flintstone mapping");
        return found->second;
    };
    auto operand = [&](const firestone::ir::Operand& o) {
        using K = firestone::ir::OperandKind;
        if (o.kind == K::Value) return Operand::ref(lookup_value(o.value));
        if (o.kind == K::Integer) return Operand::constant(o.integer, from_firestone_type(o.type));
        if (o.kind == K::Float) return Operand::real(o.floating, from_firestone_type(o.type));
        return Operand::named(o.symbol, from_firestone_type(o.type));
    };
    for (auto id : graph_blocks) {
        auto block = original.block(lookup_block(id)); block.operations.clear();
        std::size_t position = 0;
        for (const auto& instruction : graph.cfg().block(id).instructions) {
            Operation op; op.name = operation_name(instruction.opcode);
            if (any_vm) op.name = "vm." + op.name.substr(op.name.find('.') + 1);
            if (instruction.result) {
                auto value = lookup_value(*instruction.result);
                if (auto found = provenance.find(value); found != provenance.end()) op.location = found->second->location;
                op.results = {{value, from_firestone_type(instruction.type), op.location}};
            } else if (position < original.block(block.id).operations.size()) op.location = original.block(block.id).operations[position].location;
            for (const auto& o : instruction.operands) op.operands.push_back(operand(o));
            for (auto b : instruction.targets) op.successors.push_back(lookup_block(b));
            for (auto b : instruction.incoming_blocks) op.incoming.push_back(lookup_block(b));
            if (instruction.opcode == firestone::ir::Opcode::Call) op.attributes["callee"] = instruction.annotation;
            block.operations.push_back(std::move(op)); ++position;
        }
        result.blocks.push_back(std::move(block));
    }
    return result;
}
}

firestone::ir::SimpleSSAGraph to_ssa(const Module& source, const Function& input, const Registry& registry) {
    detail::check(&source.function(input.name) == &input, "ssa.function", "SSA export requires a function owned by the supplied module");
    auto module = instantiate(source, registry);
    const auto& function = module.function(input.name);
    detail::check(!function.external, "ssa.external", "external functions have no SSA body");
    firestone::ir::SimpleSSAGraph graph;
    std::map<BlockId, firestone::ir::BlockId> blocks;
    std::map<ValueId, firestone::ir::ValueId> values;
    for (const auto& block : function.blocks) blocks[block.id] = graph.cfg().create_block(block.name);
    {
        auto entry = blocks.find(function.entry);
        detail::check(entry != blocks.end(), "ssa.entry", "unknown entry block", function.location);
        graph.cfg().set_entry(entry->second);
    }
    auto lookup_block = [&](BlockId id, const Location& location) {
        auto found = blocks.find(id);
        detail::check(found != blocks.end(), "ssa.block", "unknown block ^" + std::to_string(id), location);
        return found->second;
    };
    auto lookup_value = [&](ValueId id, const Location& location) {
        auto found = values.find(id);
        detail::check(found != values.end(), "ssa.value", "undefined value %" + std::to_string(id), location);
        return found->second;
    };
    for (const auto& value : function.parameters) values[value.id] = graph.parameter(detail::firestone_type(value.type));
    for (const auto& block : function.blocks) for (const auto& op : block.operations) {
        detail::check(op.results.size() <= 1, "ssa.results", "multi-result operations require explicit scalar lowering", op.location);
        for (const auto& value : op.results) values[value.id] = graph.reserve_value(detail::firestone_type(value.type));
    }
    auto operand = [&](const Operand& o, const Location& location) -> firestone::ir::Operand {
        using F = firestone::ir::Operand;
        if (o.kind == OperandKind::Value) return F::ref(lookup_value(o.value, location));
        if (o.kind == OperandKind::Integer) return F::constant(o.integer, detail::firestone_type(o.type));
        if (o.kind == OperandKind::Float) return F::real(o.floating, detail::firestone_type(o.type));
        return F::named(o.symbol, detail::firestone_type(o.type));
    };
    for (const auto& block : function.blocks) for (const auto& op : block.operations) {
        auto code = registry.operation(op.name).ssa_opcode;
        detail::check(code.has_value(), "ssa.operation", "operation requires explicit semantic lowering: " + op.name, op.location);
        firestone::ir::Instruction instruction;
        instruction.opcode = *code;
        instruction.type = op.results.empty() ? firestone::ir::Type::void_type() : detail::firestone_type(op.results[0].type);
        if (!op.results.empty()) instruction.result = lookup_value(op.results[0].id, op.location);
        for (const auto& o : op.operands) instruction.operands.push_back(operand(o, op.location));
        for (auto b : op.successors) instruction.targets.push_back(lookup_block(b, op.location));
        for (auto b : op.incoming) instruction.incoming_blocks.push_back(lookup_block(b, op.location));
        if (*code == firestone::ir::Opcode::Call) {
            auto found = op.attributes.find("callee");
            detail::check(found != op.attributes.end() && std::holds_alternative<std::string>(found->second), "ssa.call", "SSA call needs a string callee", op.location);
            instruction.annotation = std::get<std::string>(found->second);
        }
        if (firestone::ir::is_terminator(*code)) graph.cfg().terminate(lookup_block(block.id, op.location), std::move(instruction));
        else graph.cfg().append(lookup_block(block.id, op.location), std::move(instruction));
    }
    auto errors = graph.verify();
    if (!errors.empty()) detail::fail("ssa." + errors.front().code, errors.front().message, function.location);
    return graph;
}

namespace {
// Wrapping intrinsics cover ops where Firestone's checked arithmetic would
// trap on overflow (Add/Sub/Mul/Div/Neg/Shl). Mod needs no wrapper: its result
// always fits and MIN%-1 is already defined as 0. Shr is already arithmetic
// sign-extending with no overflow case, matching modular signed semantics.
std::string intrinsic(firestone::ir::Opcode code, unsigned bits) {
    using O = firestone::ir::Opcode;
    const char* name = nullptr;
    switch (code) {
    case O::Add: name = "add"; break; case O::Sub: name = "sub"; break;
    case O::Mul: name = "mul"; break; case O::Div: name = "div"; break;
    case O::Neg: name = "neg"; break; case O::Shl: name = "shl"; break;
    default: return {};
    }
    return "__flintstone_mod_" + std::string(name) + "_i" + std::to_string(bits);
}
firestone::ir::SimpleSSAGraph modular_ssa(const firestone::ir::SimpleSSAGraph& source, firestone::vm::Module& module) {
    firestone::ir::SimpleSSAGraph result;
    auto definitions = source.definitions();
    for (std::size_t id = 0; id < source.value_count(); ++id) {
        if (definitions.at(id).parameter) result.parameter(source.type(id));
        else result.reserve_value(source.type(id));
    }
    std::map<firestone::ir::BlockId, firestone::ir::BlockId> blocks;
    for (auto b : source.cfg().blocks()) blocks[b] = result.cfg().create_block(source.cfg().block(b).name);
    result.cfg().set_entry(blocks.at(source.cfg().entry()));
    auto guard = [&](firestone::ir::BlockId block, firestone::ir::ValueId value) {
        const auto type = source.type(value);
        if (type.kind != firestone::ir::TypeKind::Integer || type.bits <= 1 || type.bits >= 64) return;
        auto symbol = "__flintstone_check_i" + std::to_string(type.bits);
        module.natives[symbol] = {{firestone::vm::ValueKind::Integer}, firestone::vm::ValueKind::Null};
        firestone::ir::Instruction check;
        check.opcode = firestone::ir::Opcode::Call; check.type = firestone::ir::Type::void_type();
        check.operands = {firestone::ir::Operand::ref(value)}; check.annotation = symbol;
        result.cfg().append(blocks.at(block), std::move(check));
    };
    for (auto b : source.cfg().blocks()) {
        if (b == source.cfg().entry()) for (const auto& [id, definition] : definitions) if (definition.parameter) guard(b, id);
        for (auto instruction : source.cfg().block(b).instructions) {
            // The cf.trap lowering emits a call to the reserved throwing
            // intrinsic. Declare it with the call's own result kind so the VM
            // compiler treats it as a native call; the runtime binding throws.
            if (instruction.opcode == firestone::ir::Opcode::Call && instruction.annotation == "__flintstone_trap") {
                firestone::vm::Signature signature;
                signature.result = firestone::vm::runtime_kind(instruction.type);
                module.natives["__flintstone_trap"] = signature;
            }
            const auto original_opcode = instruction.opcode;
            // Narrow values entering from host calls or heap loads need a
            // runtime width check; FSVM signatures alone record only scalar kinds.
            bool external_value = instruction.opcode == firestone::ir::Opcode::Call || instruction.opcode == firestone::ir::Opcode::Load;
            if (instruction.opcode == firestone::ir::Opcode::Copy) {
                detail::check(!instruction.operands.empty(), "ssa.copy", "copy requires an operand");
                external_value = instruction.operands.at(0).kind == firestone::ir::OperandKind::Symbol;
            }
            const auto produced = instruction.result;
            if (instruction.type.kind == firestone::ir::TypeKind::Integer && instruction.type.bits > 1) {
                const auto symbol = intrinsic(instruction.opcode, instruction.type.bits);
                if (!symbol.empty()) {
                    firestone::vm::Signature signature;
                    signature.result = firestone::vm::ValueKind::Integer;
                    signature.parameters.assign(instruction.operands.size(), firestone::vm::ValueKind::Integer);
                    module.natives[symbol] = signature;
                    instruction.opcode = firestone::ir::Opcode::Call; instruction.annotation = symbol;
                }
            }
            for (auto& target : instruction.targets) target = blocks.at(target);
            for (auto& predecessor : instruction.incoming_blocks) predecessor = blocks.at(predecessor);
            // Terminator status is decided by the original opcode: no wrapping
            // intrinsic targets a terminator, so a rewrite must never change it.
            if (firestone::ir::is_terminator(original_opcode)) {
                detail::check(firestone::ir::is_terminator(instruction.opcode), "ssa.terminator", "lowering changed terminator status");
                result.cfg().terminate(blocks.at(b), std::move(instruction));
            } else {
                detail::check(!firestone::ir::is_terminator(instruction.opcode), "ssa.terminator", "lowering changed terminator status");
                result.cfg().append(blocks.at(b), std::move(instruction));
            }
            if (external_value && produced) guard(b, *produced);
        }
    }
    return result;
}
}
void bind_vm_runtime(firestone::vm::Environment& environment, const firestone::vm::Module& module) {
    for (const auto& [name, signature] : module.natives) {
        if (name == "__flintstone_trap") {
            detail::check(signature.parameters.empty(), "vm.intrinsic", "trap intrinsic takes no parameters");
            environment.bind(name, signature, [](const auto&) -> firestone::vm::Value {
                throw firestone::vm::VMError("vm.trap", "explicit trap reached");
            });
            continue;
        }
        const std::string check_prefix = "__flintstone_check_i";
        if (name.rfind(check_prefix, 0) == 0) {
            unsigned bits = 0;
            auto parsed = std::from_chars(name.data() + check_prefix.size(), name.data() + name.size(), bits);
            detail::check(parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && bits >= 2 && bits < 64,
                          "vm.intrinsic", "invalid narrow-integer guard");
            const firestone::vm::Signature expected{{firestone::vm::ValueKind::Integer}, firestone::vm::ValueKind::Null};
            detail::check(signature == expected, "vm.intrinsic", "integer guard signature mismatch");
            environment.bind(name, signature, [bits](const auto& values) -> firestone::vm::Value {
                detail::check(firestone::sema::SignedRange(bits).contains(firestone::vm::require<std::int64_t>(values.at(0))),
                              "vm.width", "external integer exceeds its declared width");
                return {};
            });
            continue;
        }
        const std::string prefix = "__flintstone_mod_";
        if (name.rfind("__flintstone_", 0) != 0) continue;
        auto split = name.rfind("_i");
        detail::check(name.rfind(prefix, 0) == 0 && split != std::string::npos && split >= prefix.size(), "vm.intrinsic", "invalid intrinsic name");
        auto operation = name.substr(prefix.size(), split - prefix.size());
        unsigned bits = 0;
        auto parsed = std::from_chars(name.data() + split + 2, name.data() + name.size(), bits);
        detail::check(parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && bits >= 2 && bits <= 64 &&
                      (operation == "add" || operation == "sub" || operation == "mul" || operation == "div" || operation == "neg" || operation == "shl"),
                      "vm.intrinsic", "unsupported modular intrinsic");
        firestone::vm::Signature expected;
        expected.result = firestone::vm::ValueKind::Integer;
        expected.parameters.assign(operation == "neg" ? 1 : 2, firestone::vm::ValueKind::Integer);
        detail::check(signature == expected, "vm.intrinsic", "intrinsic signature mismatch");
        environment.bind(name, signature, [operation, bits](const std::vector<firestone::vm::Value>& values) -> firestone::vm::Value {
            auto a = firestone::vm::require<std::int64_t>(values.at(0));
            auto b = values.size() > 1 ? firestone::vm::require<std::int64_t>(values.at(1)) : 0;
            firestone::sema::SignedRange range(bits);
            detail::check(range.contains(a) && (operation == "shl" || operation == "neg" || range.contains(b)), "vm.intrinsic", "intrinsic operand exceeds its width");
            const auto mask = bits == 64 ? UINT64_MAX : (std::uint64_t(1) << bits) - 1;
            auto u = static_cast<std::uint64_t>(a), v = static_cast<std::uint64_t>(b), answer = std::uint64_t(0);
            if (operation == "add") answer = u + v;
            else if (operation == "sub") answer = u - v;
            else if (operation == "mul") answer = u * v;
            else if (operation == "neg") answer = std::uint64_t(0) - u;
            else if (operation == "shl") {
                detail::check(b >= 0 && static_cast<std::uint64_t>(b) < bits, "vm.shift", "invalid shift count"); answer = u << b;
            } else {
                detail::check(b != 0, "vm.divzero", "division by zero");
                if (a == range.minimum && b == -1) answer = static_cast<std::uint64_t>(a);
                else answer = static_cast<std::uint64_t>(a / b);
            }
            answer &= mask;
            if (bits < 64 && (answer & (std::uint64_t(1) << (bits - 1)))) answer |= ~mask;
            return std::bit_cast<std::int64_t>(answer);
        });
    }
}
firestone::vm::Module compile_vm(const Module& source, const Registry& registry, firestone::vm::VMModel model) {
    auto module = instantiate(source, registry);
    detail::check(module.target.name == "vm", "vm.target", "VM execution requires target vm; retarget explicitly before execution");
    if (module.level == Level::High) lower(module, registry, Level::Core);
    using namespace firestone;
    auto signature = [](const Function& f) {
        vm::Signature result; result.result = vm::runtime_kind(detail::firestone_type(f.result));
        for (const auto& p : f.parameters) result.parameters.push_back(vm::runtime_kind(detail::firestone_type(p.type)));
        return result;
    };
    std::map<std::string, vm::Index> indices;
    for (const auto& f : module.functions) if (!f.external) indices[f.name] = indices.size();
    vm::Module result;
    for (const auto& f : module.functions) if (f.external) result.natives[f.name] = signature(f);
    // Compile each definition independently with declared external signatures,
    // then link internal calls. This supports forward and mutually recursive calls
    // without weakening Firestone's transactional module verifier.
    for (const auto& f : module.functions) if (!f.external) {
        vm::Module unit;
        for (const auto& other : module.functions) if (other.name != f.name) unit.natives[other.name] = signature(other);
        auto scalar = to_ssa(module, f, registry);
        auto address_space = [](const firestone::ir::Type& type) {
            detail::check(type.kind != firestone::ir::TypeKind::Pointer || type.name == "generic", "vm.address-space", "VM pointers require the generic heap-cell address space");
        };
        for (std::size_t v = 0; v < scalar.value_count(); ++v) address_space(scalar.type(v));
        for (auto block : scalar.cfg().blocks()) for (const auto& i : scalar.cfg().block(block).instructions)
            for (const auto& o : i.operands) if (o.kind != firestone::ir::OperandKind::Value) address_space(o.type);
        auto graph = modular_ssa(scalar, unit);
        firestone::vm::Index id = 0;
        try {
            id = vm::compile(unit, graph, f.name, model);
        } catch (const Error&) { throw; }
        catch (const std::exception& e) { detail::fail("vm.lowering", std::string("Firestone lowering failed: ") + e.what(), f.location); }
        for (const auto& [name, s] : unit.natives) if (name.rfind("__flintstone_", 0) == 0) result.natives[name] = s;
        auto function = unit.function(id);
        detail::check(function.signature == signature(f), "vm.signature", "VM lowering requires a return matching the declared signature", f.location);
        std::vector<vm::Index> constants;
        for (const auto& value : unit.constants.values()) constants.push_back(result.constants.add(value));
        detail::check(constants.size() == unit.constants.size(), "vm.constants", "constant remapping failed", f.location);
        for (vm::Index pc = 0; pc < function.code.size(); ++pc) {
            auto& instruction = function.code.at(pc);
            if (instruction.opcode == vm::Opcode::Constant) {
                detail::check(instruction.index < constants.size(), "vm.constants", "unknown constant reference", f.location);
                instruction.index = constants.at(instruction.index);
            } else if (instruction.opcode == vm::Opcode::Call) {
                // In a per-function unit, a direct Call can only be self-recursion:
                // every other function is pre-declared as a native (see above).
                // Verify the invariant instead of silently retargeting.
                detail::check(instruction.index == 0, "vm.link", "unexpected non-self call in isolated unit", f.location);
                auto self = indices.find(f.name);
                detail::check(self != indices.end(), "vm.link", "unknown function identity", f.location);
                instruction.index = self->second;
            } else if (instruction.opcode == vm::Opcode::NativeCall && indices.count(instruction.symbol)) {
                instruction.opcode = vm::Opcode::Call; instruction.index = indices.at(instruction.symbol); instruction.symbol.clear();
            }
        }
        result.add_function(std::move(function));
    }
    try {
        vm::require_valid(result);
    } catch (const std::exception& e) { detail::fail("vm.verification", std::string("VM verification failed: ") + e.what()); }
    return result;
}
firestone::vm::Value execute(const Module& source, const Registry& registry, const std::string& name,
                            const std::vector<firestone::vm::Value>& arguments, std::size_t budget,
                            firestone::vm::Environment* environment) {
    auto module = instantiate(source, registry);
    const auto& function = module.function(name);
    detail::check(!function.external && function.parameters.size() == arguments.size(), "vm.arguments", "invalid entry function or argument count");
    for (std::size_t n = 0; n < arguments.size(); ++n) {
        const auto& type = function.parameters[n].type;
        if (type.kind == TypeKind::Integer && type.bits == 1) {
            auto boolean = std::get_if<bool>(&arguments[n]);
            detail::check(boolean != nullptr, "vm.arguments", "i1 argument requires a boolean value");
        } else if (type.kind == TypeKind::Integer && type.bits < 64) {
            auto integer = std::get_if<std::int64_t>(&arguments[n]);
            detail::check(integer && *integer >= -(std::int64_t(1) << (type.bits - 1)) && *integer < (std::int64_t(1) << (type.bits - 1)),
                          "vm.arguments", "argument exceeds its declared integer width");
        } else if (type.kind == TypeKind::Integer) {
            detail::check(std::holds_alternative<std::int64_t>(arguments[n]), "vm.arguments", "integer argument requires an integer value");
        } else if (type.kind == TypeKind::Float) {
            auto real = std::get_if<double>(&arguments[n]);
            detail::check(real && std::isfinite(*real), "vm.arguments", "float argument requires a finite f64 value");
        } else if (type.kind == TypeKind::Pointer) {
            detail::check(std::holds_alternative<firestone::vm::Address>(arguments[n]), "vm.arguments", "pointer argument requires a VM address");
        } else {
            detail::fail("vm.arguments", "unsupported entry parameter type: " + print(type));
        }
    }
    auto bytecode = compile_vm(module, registry);
    firestone::vm::Environment local;
    bind_vm_runtime(environment ? *environment : local, bytecode);
    firestone::vm::ExecutionLimits limits; limits.instructions = budget;
    firestone::vm::ExecutionResult result{{}, {}};
    try {
        result = firestone::vm::RegisterVM{}.run(bytecode, bytecode.find(name), arguments, environment ? *environment : local, limits);
    } catch (const Error&) { throw; }
    catch (const firestone::vm::VMError& e) { detail::fail(std::string("vm.") + e.code, e.what()); }
    catch (const std::exception& e) { detail::fail("vm.execution", std::string("VM execution failed: ") + e.what()); }
    // Firestone traps throw VMError above; a returned non-Returned stop can only
    // be budget exhaustion (ExecutionStop has only Returned/Budget).
    detail::check(result.stop == firestone::vm::ExecutionStop::Returned && result.state.result.has_value(), "vm.budget", "instruction budget exhausted");
    return std::move(*result.state.result);
}
} // namespace flintstone
