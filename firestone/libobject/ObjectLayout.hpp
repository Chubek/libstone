#pragma once
#include "ClassHierarchy.hpp"
#include "../libtype/TypeLayout.hpp"

namespace firestone::object {

struct ObjectLayout {
    std::size_t size = 0, alignment = 1;
    std::map<std::string, std::size_t> offsets;
    std::optional<std::size_t> vptr_offset;
};

// Flat storage (not native base subobjects): one leading vptr, if needed, and
// unique fields from each base in declaration order, followed by local fields.
// Later additions append to each existing layout; historical revisions remain queryable.
inline ObjectLayout object_layout_at(const ClassHierarchy& hierarchy, ClassId id, ClassLayoutRevision revision,
                                     type::LayoutPolicy policy = {}) {
    auto schema = hierarchy.layout_schema(id, revision);
    auto& fields = schema.fields;
    if (schema.polymorphic) fields.insert(fields.begin(), {"$vptr", type::Type::pointer(type::Type::atomic(ir::Type::void_type()))});
    auto storage = type::layout(type::Type::record(fields), policy);
    ObjectLayout result{storage.size, storage.alignment, {}, {}};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].first == "$vptr") result.vptr_offset = storage.offsets[i];
        else result.offsets[fields[i].first] = storage.offsets[i];
    }
    return result;
}

inline ObjectLayout object_layout(const ClassHierarchy& hierarchy, ClassId id, type::LayoutPolicy policy = {}) {
    return object_layout_at(hierarchy, id, hierarchy.layout_revision(id), policy);
}

} // namespace firestone::object
