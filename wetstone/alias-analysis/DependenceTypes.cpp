#include "DependenceTypes.hpp"

namespace wetstone::alias::detail {
namespace {
bool may_read(AccessKind kind) { return kind != AccessKind::Write; }
bool may_write(AccessKind kind) { return kind != AccessKind::Read; }
}

void append_dependences(DependenceResult& result, const MemoryAccess& source,
                        const MemoryAccess& sink, IterationRelation relation,
                        AliasResult alias_result) {
    if (alias_result == AliasResult::NoAlias) return;
    const auto distance = relation == IterationRelation::SameIteration
        ? std::optional<std::size_t>{0} : std::nullopt;
    const auto append = [&](DependenceKind kind) {
        result.dependences.push_back({source, sink, kind, relation, distance, alias_result});
    };
    if (may_write(source.kind) && may_read(sink.kind)) append(DependenceKind::Flow);
    if (may_read(source.kind) && may_write(sink.kind)) append(DependenceKind::Anti);
    if (may_write(source.kind) && may_write(sink.kind)) append(DependenceKind::Output);
}
} // namespace wetstone::alias::detail
