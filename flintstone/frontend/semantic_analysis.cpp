#include "../internal.hpp"
#include <cmath>

namespace flintstone {
namespace {
void type_check(const Type& type, const Module& module, const Registry& registry, bool value, unsigned depth = 0) {
    using detail::check;
    check(depth < 64, "type.depth", "type nesting limit exceeded");
    check(static_cast<unsigned>(type.kind) <= static_cast<unsigned>(TypeKind::Parameter), "type.kind", "invalid type kind");
    check(type.elements.size() <= 100000, "resource.type", "type element count limit exceeded");
    if (type.kind == TypeKind::Void) check(!value, "type.void", "void is not a value type");
    if (type.kind == TypeKind::Integer || type.kind == TypeKind::Float)
        check(type.bits && type.bits <= 65536, "type.width", "scalar width must be in 1..65536");
    else check(!type.bits, "type.width", "nonscalar type carries a scalar width");
    if (type.kind == TypeKind::Vector)
        check(type.count && type.count <= 1000000 && type.elements.size() == 1 &&
                  type.elements[0].kind != TypeKind::Tuple && type.elements[0].kind != TypeKind::Vector,
              "type.vector", "vector requires a positive bounded count and a scalar element");
    else check(!type.count, "type.count", "only vectors have an element count");
    if (type.kind != TypeKind::Vector && type.kind != TypeKind::Tuple && type.kind != TypeKind::Opaque)
        check(type.elements.empty(), "type.elements", "unexpected type arguments");
    if (type.kind == TypeKind::Pointer) check(detail::valid_name(type.name), "type.pointer", "invalid address-space name");
    else if (type.kind == TypeKind::Opaque) {
        check(detail::valid_name(type.name), "type.opaque", "invalid opaque type name");
        auto dialect = detail::dialect_name(type.name);
        const auto& def = registry.dialect(dialect);
        check(def.verify_type && def.verify_type(type, registry.parameters(module, dialect), module.target),
              "type.opaque", "unrecognized or invalid dialect type " + type.name);
    } else check(type.name.empty(), "type.name", "unexpected type name or unresolved parameter");
    for (const auto& element : type.elements) type_check(element, module, registry, true, depth + 1);
}
void attributes_check(const Attributes& attrs, const Module& module, const Registry& registry) {
    detail::check(attrs.size() <= 100000, "resource.attributes", "attribute count limit exceeded");
    for (const auto& [key, value] : attrs) {
        detail::check(detail::valid_name(key), "attribute.name", "invalid attribute name '" + key + "'");
        if (auto number = std::get_if<double>(&value)) detail::check(std::isfinite(*number), "attribute.float", "nonfinite attribute '" + key + "'");
        if (auto text = std::get_if<std::string>(&value)) {
            detail::check(text->size() <= max_string_bytes, "resource.attributes", "attribute string length limit exceeded");
            for (unsigned char byte : *text)
                detail::check(byte >= 32 || byte == '\n' || byte == '\r' || byte == '\t', "attribute.string",
                              "unsupported control byte in string attribute '" + key + "'");
        }
        if (auto type = std::get_if<Type>(&value)) type_check(*type, module, registry, false);
    }
}
}
namespace detail {
CFG cfg(const Function& function, const Registry& registry) {
    CFG result;
    for (const auto& block : function.blocks) {
        check(block.id != invalid_id && !result.blocks.count(block.id), "cfg.block",
              "invalid or duplicate block ID ^" + std::to_string(block.id));
        auto id = result.graph.create_block(block.name); result.blocks[block.id] = id; result.original[id] = block.id;
    }
    {
        auto entry = result.blocks.find(function.entry);
        check(entry != result.blocks.end(), "cfg.entry", "function has no valid entry block");
        result.graph.set_entry(entry->second);
    }
    for (const auto& block : function.blocks) {
        check(!block.operations.empty(), "cfg.terminator", "block has no terminator", block.location);
        for (std::size_t n = 0; n < block.operations.size(); ++n) {
            const auto& operation = block.operations[n];
            const OperationDefinition* definition = nullptr;
            try {
                definition = &registry.operation(operation.name);
            } catch (const std::exception& e) {
                fail("operation.unknown", std::string("unregistered operation ") + operation.name + ": " + e.what(), operation.location);
            }
            check(definition->terminator == (n + 1 == block.operations.size()), "cfg.terminator", "terminator must be the last operation", operation.location);
            if (!definition->terminator) { check(operation.successors.empty(), "cfg.successor", "nonterminator has successors", operation.location); continue; }
            check(operation.successors.size() <= 2, "cfg.successor", "this CFG supports at most two explicit successors", operation.location);
            for (auto id : operation.successors)
                check(result.blocks.count(id), "cfg.successor", "unknown successor block ^" + std::to_string(id), operation.location);
            auto id = result.blocks.find(block.id)->second;
            auto target = [&](BlockId original) {
                auto found = result.blocks.find(original);
                check(found != result.blocks.end(), "cfg.successor",
                      "unknown successor block ^" + std::to_string(original), operation.location);
                return found->second;
            };
            if (operation.successors.empty()) result.graph.return_value(id);
            else if (operation.successors.size() == 1) result.graph.jump(id, target(operation.successors[0]));
            else result.graph.branch(id, firestone::ir::Operand::constant(1, firestone::ir::Type::boolean()),
                                     target(operation.successors[0]), target(operation.successors[1]));
        }
    }
    return result;
}
}

Diagnostics verify(const Module& source, const Registry& registry) {
    Diagnostics errors;
    auto record = [&](const std::exception& e, Location location = {}, std::string function = {}, BlockId block = invalid_id, std::size_t op = SIZE_MAX) {
        if (const auto* error = dynamic_cast<const Error*>(&e)) {
            for (auto diagnostic : error->diagnostics()) {
                if (diagnostic.location.file.empty()) diagnostic.location = location;
                if (diagnostic.function.empty()) diagnostic.function = function;
                if (diagnostic.block == invalid_id) diagnostic.block = block;
                if (diagnostic.operation == SIZE_MAX) diagnostic.operation = op;
                errors.push_back(std::move(diagnostic));
            }
        } else errors.push_back({"validation.callback", e.what(), location, function, block, op});
    };
    Module module;
    try {
        using detail::check;
        check(detail::valid_name(source.name), "module.name", "invalid module name '" + source.name + "'");
        check(static_cast<unsigned>(source.level) <= static_cast<unsigned>(Level::Machine), "module.level", "invalid module level");
        const auto& target = source.target;
        check(!target.name.empty(), "target.name", "target name is empty");
        check(detail::valid_name(target.name), "target.name", "invalid target name '" + target.name + "'");
        check(target.pointer_bits >= 8 && target.pointer_bits <= 128 && !(target.pointer_bits & (target.pointer_bits - 1)),
              "target.layout", "pointer width must be a power of two in 8..128");
        check(target.index_bits >= 2 && target.index_bits <= 64, "target.layout", "index width must be in 2..64");
        check(target.endian == Endian::Little || target.endian == Endian::Big, "target.endian", "invalid target endianness");
        check(target.features.size() <= 100000 && target.parameters.size() <= 100000, "resource.target", "target feature/parameter limit exceeded");
        for (const auto& feature : target.features)
            check(detail::valid_name(feature), "target.feature", "invalid feature name '" + feature + "'");
        for (const auto& [name, value] : target.parameters) {
            (void)value;
            check(name != "pointer_bits" && name != "index_bits" && name != "endian" && name.rfind("feature.", 0) != 0,
                  "target.parameter", "target parameter collides with a reserved layout/feature key");
        }
        for (const auto& [name, type] : source.type_bindings) { (void)type; check(detail::valid_name(name), "type.parameter", "invalid type parameter name"); }
        check(source.imports.empty(), "import.unresolved",
              "unresolved import '" + (source.imports.empty() ? std::string() : source.imports.front()) +
                  "'; call resolve_imports before verification");
        module = detail::specialize(source);
        for (const auto& [name, type] : module.type_bindings) { (void)name; type_check(type, module, registry, false); }
        attributes_check(module.attributes, module, registry); attributes_check(target.parameters, module, registry);
        for (const auto& [name, attrs] : module.dialects) { attributes_check(attrs, module, registry); registry.parameters(module, name); }
        check(module.functions.size() <= 100000, "resource.functions", "function count limit exceeded");
        std::set<std::string> names;
        for (const auto& f : module.functions) {
            check(detail::valid_name(f.name) && names.insert(f.name).second, "symbol.duplicate",
                  "invalid or duplicate function name @" + f.name, f.location);
            check(f.name.rfind("__flintstone_", 0) != 0, "symbol.reserved", "__flintstone_ is reserved for runtime intrinsics", f.location);
            check(f.parameters.size() <= 100000, "resource.function", "parameter count limit exceeded", f.location);
        }
    } catch (const std::exception& e) { record(e); return errors; }
    for (const auto& function : module.functions) {
        using detail::check;
        struct Definition { Type type; BlockId block; std::size_t operation; bool parameter; };
        std::map<ValueId, Definition> definitions;
        std::map<ValueId, Type> types;
        try {
            type_check(function.result, module, registry, false);
            attributes_check(function.attributes, module, registry);
            std::size_t operation_count = 0;
            for (const auto& b : function.blocks) {
                detail::check(b.operations.size() <= 1000000, "resource.function", "block operation limit exceeded", b.location);
                operation_count += b.operations.size();
                detail::check(operation_count <= 1000000, "resource.function", "function size limit exceeded", function.location);
            }
            check(function.blocks.size() <= 10000, "resource.function", "function block limit exceeded");
            auto define = [&](const Value& value, BlockId block, std::size_t position, bool parameter) {
                type_check(value.type, module, registry, true);
                check(value.id != invalid_id && definitions.emplace(value.id, Definition{value.type, block, position, parameter}).second,
                      "ssa.definition", "invalid or multiply-defined value %" + std::to_string(value.id), value.location);
                types[value.id] = value.type;
            };
            for (const auto& parameter : function.parameters) define(parameter, invalid_id, 0, true);
            if (function.external) {
                check(function.blocks.empty() && function.entry == invalid_id, "extern.body", "external function cannot have a body");
                continue;
            }
            for (const auto& b : function.blocks) for (std::size_t n = 0; n < b.operations.size(); ++n)
                for (const auto& value : b.operations[n].results) define(value, b.id, n, false);
        } catch (const std::exception& e) { record(e, function.location, function.name); continue; }
        detail::CFG cfg;
        firestone::ir::DominatorSets dominators;
        try {
            cfg = detail::cfg(function, registry);
            check(cfg.graph.predecessors(cfg.graph.entry()).empty(), "cfg.entry", "entry block cannot have incoming CFG edges");
            check(cfg.graph.unreachable_blocks().empty(), "cfg.unreachable", "unreachable blocks must be removed or connected explicitly");
            try {
                dominators = cfg.graph.dominators();
            } catch (const std::exception& e) {
                detail::fail("cfg.dominators", std::string("dominator computation failed: ") + e.what());
            }
        } catch (const std::exception& e) { record(e, function.location, function.name); continue; }
        for (const auto& block : function.blocks) {
            bool past_phi = false;
            for (std::size_t n = 0; n < block.operations.size(); ++n) {
                const auto& op = block.operations[n];
                try {
                    const auto& def = registry.operation(op.name);
                    check(static_cast<unsigned>(def.level) >= static_cast<unsigned>(module.level), "level.operation",
                          "operation " + op.name + " is above the module's IL level", op.location);
                    check(op.operands.size() >= def.min_operands && op.operands.size() <= def.max_operands &&
                          op.results.size() >= def.min_results && op.results.size() <= def.max_results,
                          "operation.arity", "incorrect operand or result count for " + op.name + ": got " +
                              std::to_string(op.operands.size()) + " operands and " + std::to_string(op.results.size()) +
                              " results, expected [" + std::to_string(def.min_operands) + ".." + std::to_string(def.max_operands) +
                              "] operands and [" + std::to_string(def.min_results) + ".." + std::to_string(def.max_results) + "] results");
                    check(op.operands.size() <= 100000 && op.results.size() <= 100000 && op.successors.size() <= 2 &&
                              op.incoming.size() <= 100000,
                          "resource.operands", "operation list limit exceeded", op.location);
                    auto params = registry.parameters(module, detail::dialect_name(op.name));
                    attributes_check(op.attributes, module, registry);
                    if (def.phi) {
                        check(!past_phi, "ssa.phi", "phi must precede non-phi operations", op.location);
                        auto block_it = cfg.blocks.find(block.id);
                        check(block_it != cfg.blocks.end(), "ssa.phi", "unknown block ^" + std::to_string(block.id), op.location);
                        std::set<BlockId> labels(op.incoming.begin(), op.incoming.end()), predecessors;
                        for (auto id : cfg.graph.predecessors(block_it->second)) {
                            auto original = cfg.original.find(id);
                            check(original != cfg.original.end(), "ssa.phi", "unknown CFG predecessor", op.location);
                            predecessors.insert(original->second);
                        }
                        check(!labels.empty() && labels == predecessors && labels.size() == op.incoming.size() && op.incoming.size() == op.operands.size(),
                              "ssa.phi", "phi in ^" + std::to_string(block.id) + " must cover every predecessor exactly once", op.location);
                    } else { past_phi = true; check(op.incoming.empty(), "ssa.phi", "non-phi operation has incoming labels", op.location); }
                    for (std::size_t p = 0; p < op.operands.size(); ++p) {
                        const auto& operand = op.operands[p];
                        check(static_cast<unsigned>(operand.kind) <= static_cast<unsigned>(OperandKind::Symbol), "operand.kind", "invalid operand kind");
                        if (operand.kind != OperandKind::Value) {
                            type_check(operand.type, module, registry, true);
                            if (operand.kind == OperandKind::Integer) {
                                check(operand.type.kind == TypeKind::Integer, "literal.type", "integer literal has noninteger type");
                                auto bits = operand.type.bits;
                                check(bits >= 1, "literal.range", "integer literal has zero width");
                                if (bits == 1) check(operand.integer == 0 || operand.integer == 1, "literal.range", "i1 literal must be zero or one");
                                else if (bits < 64) check(operand.integer >= -(std::int64_t(1) << (bits - 1)) && operand.integer < (std::int64_t(1) << (bits - 1)),
                                                          "literal.range", "integer literal exceeds signed bit-pattern range");
                            } else if (operand.kind == OperandKind::Float) {
                                check(operand.type == Type::floating(64) && std::isfinite(operand.floating), "literal.float", "floating literal requires finite f64");
                            } else check(detail::valid_name(operand.symbol), "symbol.name", "invalid symbol operand name '" + operand.symbol + "'");
                            continue;
                        }
                        auto found = definitions.find(operand.value);
                        check(found != definitions.end(), "ssa.undefined", "undefined value %" + std::to_string(operand.value));
                        const auto& d = found->second;
                        if (d.parameter) continue;
                        check(!def.phi || p < op.incoming.size(), "ssa.phi", "phi operand lacks an incoming label", op.location);
                        auto use_block = def.phi ? op.incoming[p] : block.id;
                        std::size_t position = n;
                        if (def.phi) {
                            bool has_block = false;
                            for (const auto& b : function.blocks) if (b.id == use_block) { position = b.operations.size(); has_block = true; break; }
                            check(has_block, "ssa.phi", "phi references unknown block ^" + std::to_string(use_block), op.location);
                        }
                        auto use_it = cfg.blocks.find(use_block);
                        auto def_it = cfg.blocks.find(d.block);
                        check(use_it != cfg.blocks.end() && def_it != cfg.blocks.end(), "ssa.dominance",
                              "definition does not dominate use of %" + std::to_string(operand.value), op.location);
                        auto dom_it = dominators.find(use_it->second);
                        check(dom_it != dominators.end() && dom_it->second.count(def_it->second) &&
                              (d.block != use_block || d.operation < position), "ssa.dominance",
                              "definition does not dominate use of %" + std::to_string(operand.value), op.location);
                    }
                    if (def.verify) def.verify(op, {module, function, registry, types, std::move(params)});
                } catch (const std::exception& e) { record(e, op.location, function.name, block.id, n); }
            }
        }
    }
    return errors;
}
void require_valid(const Module& module, const Registry& registry) {
    auto errors = verify(module, registry); if (!errors.empty()) throw Error(std::move(errors));
}
Module instantiate(const Module& source, const Registry& registry) {
    require_valid(source, registry);
    auto result = detail::specialize(source); require_valid(result, registry); return result;
}
} // namespace flintstone
