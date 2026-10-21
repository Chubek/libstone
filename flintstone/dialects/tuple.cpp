#include "../internal.hpp"

namespace flintstone {
namespace detail {
namespace {

bool scalar_field(const Type& type) {
    return type.kind == TypeKind::Integer || type.kind == TypeKind::Float || type.kind == TypeKind::Pointer;
}

} // namespace

void install_tuple_dialect(Registry& registry) {
    DialectDefinition dialect;
    dialect.name = "tuple";
    dialect.version = 1;
    OperationDefinition build;
    build.name = "tuple.build";
    build.level = Level::High;
    build.min_operands = 1;
    build.max_operands = 64;
    build.min_results = build.max_results = 1;
    build.effect = Effect::Pure;
    // No ssa_opcode: aggregates are eliminated by tuple_sroa_pass. A direct
    // lowering would need an aggregate runtime value the VM does not have.
    build.verify = [](const Operation& op, const OperationContext& context) {
        if (!op.attributes.empty()) fail("attribute.unknown", "tuple.build accepts no attributes", op.location);
        const auto& result = op.results.at(0).type;
        check(result.kind == TypeKind::Tuple, "tuple.type", "tuple.build requires a tuple result type", op.location);
        check(result.elements.size() == op.operands.size() && !result.elements.empty(), "tuple.arity",
              "tuple.build field count must match its result type", op.location);
        for (std::size_t n = 0; n < op.operands.size(); ++n) {
            const auto field = context.type(op.operands[n]);
            check(scalar_field(field), "tuple.field", "tuple fields must be integer, float or pointer scalars", op.location);
            check(field == result.elements[n], "tuple.type", "tuple.build operand type differs from its field type", op.location);
        }
    };
    OperationDefinition extract;
    extract.name = "tuple.extract";
    extract.level = Level::High;
    extract.min_operands = extract.max_operands = 1;
    extract.min_results = extract.max_results = 1;
    extract.effect = Effect::Pure;
    extract.verify = [](const Operation& op, const OperationContext& context) {
        if (op.attributes.size() != 1 || !op.attributes.count("index") ||
            !std::holds_alternative<std::int64_t>(op.attributes.at("index")))
            fail("tuple.index", "tuple.extract requires exactly one integer index attribute", op.location);
        const auto index = std::get<std::int64_t>(op.attributes.at("index"));
        const auto& source = context.type(op.operands.at(0));
        check(source.kind == TypeKind::Tuple && !source.elements.empty(), "tuple.type", "tuple.extract requires a tuple operand", op.location);
        check(index >= 0 && static_cast<std::size_t>(index) < source.elements.size(), "tuple.index", "tuple field index is out of range",
              op.location);
        check(op.results.at(0).type == source.elements[static_cast<std::size_t>(index)], "tuple.type",
              "tuple.extract result must match its field type", op.location);
    };
    dialect.operations.push_back(std::move(build));
    dialect.operations.push_back(std::move(extract));
    registry.add_dialect(std::move(dialect));
}

} // namespace detail
} // namespace flintstone
