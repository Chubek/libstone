#include "../internal.hpp"

namespace flintstone {
namespace detail {
namespace {

bool scalar_element(const Type& type) {
    if (type.kind == TypeKind::Integer) return type.bits >= 2 && type.bits <= 64;
    return type == Type::floating();
}

void check_vector(const Type& type, const Location& location) {
    check(type.kind == TypeKind::Vector, "vector.type", "vector operation requires a vector type", location);
    check(type.count >= 1 && type.count <= 256, "vector.width", "vector lane count must be in 1..256", location);
    check(type.elements.size() == 1 && scalar_element(type.elements[0]), "vector.element",
          "vector elements must be i2..i64 or f64", location);
}

OperationDefinition vector_op(std::string name, std::size_t inputs) {
    OperationDefinition def;
    def.name = std::move(name);
    def.level = Level::High;
    def.min_operands = def.max_operands = inputs;
    def.min_results = def.max_results = 1;
    def.effect = Effect::Pure;
    // Like scalar division/remainder, these may trap even when unused.
    if (def.name == "vector.div" || def.name == "vector.mod") def.effect = Effect::Unknown;
    // No ssa_opcode: vectors are eliminated by vector_scalarize_pass before
    // scalar lowering. Direct lowering would silently drop lane semantics.
    def.verify = [](const Operation& op, const OperationContext& context) {
        if (!op.attributes.empty()) fail("attribute.unknown", "vector operations accept no attributes", op.location);
        const auto& result = op.results.at(0).type;
        check_vector(result, op.location);
        for (const auto& input : op.operands)
            check(context.type(input) == (op.name == "vector.splat" ? result.elements[0] : result), "vector.type",
                  "vector operand/element types must agree", op.location);
        if (op.name == "vector.mod") check(result.elements[0].kind == TypeKind::Integer, "vector.integer", "vector.mod requires integers", op.location);
    };
    return def;
}

void check_extract(const Operation& op, const OperationContext& context) {
    if (op.attributes.size() != 1 || !op.attributes.count("lane") ||
        !std::holds_alternative<std::int64_t>(op.attributes.at("lane")))
        fail("vector.lane", "vector.extract requires exactly one integer lane attribute", op.location);
    const auto lane = std::get<std::int64_t>(op.attributes.at("lane"));
    const auto& source = context.type(op.operands.at(0));
    check_vector(source, op.location);
    check(lane >= 0 && static_cast<std::uint64_t>(lane) < source.count, "vector.lane", "extract lane is out of range", op.location);
    check(op.results.at(0).type == source.elements[0], "vector.type", "extract result must match the element type", op.location);
}

} // namespace

void install_vector_dialect(Registry& registry) {
    DialectDefinition dialect;
    dialect.name = "vector";
    dialect.version = 1;
    dialect.operations.push_back(vector_op("vector.splat", 1));
    dialect.operations.push_back(vector_op("vector.add", 2));
    dialect.operations.push_back(vector_op("vector.sub", 2));
    dialect.operations.push_back(vector_op("vector.mul", 2));
    dialect.operations.push_back(vector_op("vector.div", 2));
    dialect.operations.push_back(vector_op("vector.mod", 2));
    dialect.operations.push_back(vector_op("vector.neg", 1));
    OperationDefinition extract;
    extract.name = "vector.extract";
    extract.level = Level::High;
    extract.min_operands = extract.max_operands = 1;
    extract.min_results = extract.max_results = 1;
    extract.effect = Effect::Pure;
    extract.verify = check_extract;
    dialect.operations.push_back(std::move(extract));
    registry.add_dialect(std::move(dialect));
}

} // namespace detail
} // namespace flintstone
