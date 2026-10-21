#include "../internal.hpp"

namespace flintstone {
namespace detail {

// The pointer and atomic dialects are Core-level scalar operations for native
// targets. The portable VM models checked heap cells, not byte-addressed
// native memory, so these operations have no VM lowering: executing them
// through compile_vm/execute fails explicitly (lowering.missing /
// ssa.operation) instead of silently computing a wrong address.
void install_ptr_dialect(Registry& registry) {
    DialectDefinition ptr;
    ptr.name = "ptr";
    ptr.version = 1;
    OperationDefinition add;
    add.name = "ptr.add";
    add.level = Level::Core;
    add.min_operands = add.max_operands = 2;
    add.min_results = add.max_results = 1;
    add.effect = Effect::Pure;
    add.verify = [](const Operation& op, const OperationContext& context) {
        if (!op.attributes.empty()) fail("attribute.unknown", "ptr.add accepts no attributes", op.location);
        const auto base = context.type(op.operands.at(0));
        const auto offset = context.type(op.operands.at(1));
        check(base.kind == TypeKind::Pointer, "ptr.address", "ptr.add base must have pointer type", op.location);
        check(offset.kind == TypeKind::Integer, "ptr.offset", "ptr.add offset must have integer type", op.location);
        check(op.results.at(0).type == base, "ptr.type", "ptr.add result must match its base pointer type", op.location);
    };
    ptr.operations.push_back(std::move(add));
    registry.add_dialect(std::move(ptr));

    DialectDefinition atomic;
    atomic.name = "atomic";
    atomic.version = 1;
    OperationDefinition load;
    load.name = "atomic.load";
    load.level = Level::Core;
    load.min_operands = load.max_operands = 1;
    load.min_results = load.max_results = 1;
    load.effect = Effect::Read;
    load.verify = [](const Operation& op, const OperationContext& context) {
        const auto address = context.type(op.operands.at(0));
        check(address.kind == TypeKind::Pointer && address.name == "generic", "memory.address-space",
              "atomic memory requires ptr<generic>", op.location);
        const auto found = op.attributes.find("ordering");
        const auto* ordering = found == op.attributes.end() ? nullptr : std::get_if<std::string>(&found->second);
        check(op.attributes.size() == 1 && ordering &&
                  (*ordering == "relaxed" || *ordering == "acquire" || *ordering == "seq_cst"),
              "atomic.ordering", "atomic.load ordering must be relaxed, acquire or seq_cst", op.location);
    };
    OperationDefinition store;
    store.name = "atomic.store";
    store.level = Level::Core;
    store.min_operands = store.max_operands = 2;
    store.min_results = store.max_results = 0;
    store.effect = Effect::Write;
    store.verify = [](const Operation& op, const OperationContext& context) {
        const auto address = context.type(op.operands.at(0));
        check(address.kind == TypeKind::Pointer && address.name == "generic", "memory.address-space",
              "atomic memory requires ptr<generic>", op.location);
        const auto found = op.attributes.find("ordering");
        const auto* ordering = found == op.attributes.end() ? nullptr : std::get_if<std::string>(&found->second);
        check(op.attributes.size() == 1 && ordering &&
                  (*ordering == "relaxed" || *ordering == "release" || *ordering == "seq_cst"),
              "atomic.ordering", "atomic.store ordering must be relaxed, release or seq_cst", op.location);
    };
    atomic.operations.push_back(std::move(load));
    atomic.operations.push_back(std::move(store));
    registry.add_dialect(std::move(atomic));
}

} // namespace detail
} // namespace flintstone
