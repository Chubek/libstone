#pragma once
#include "VTable.hpp"
#include "../libtype/Imperative.hpp"
#include <initializer_list>

namespace firestone::object {

using ClassId = std::size_t;
using ClassLayoutRevision = std::size_t; // Local to one class; revision zero is its creation layout.
using ClassField = std::pair<std::string, type::Type>;

struct ClassDefinition {
    std::string name;
    std::optional<ClassId> parent; // Primary base, retained for single-base callers.
    std::vector<ClassField> fields;
    VTable vtable;
    bool frozen = false;
    std::vector<ClassId> parents = {};
};

struct ClassLayoutSchema {
    std::vector<ClassField> fields;
    bool polymorphic = false;
};

class ClassHierarchy {
public:
    ClassId create(std::string name, std::optional<ClassId> parent = {}) {
        return create(std::move(name), parent ? std::vector<ClassId>{*parent} : std::vector<ClassId>{});
    }
    ClassId create(std::string name, std::initializer_list<ClassId> parents) {
        return create(std::move(name), std::vector<ClassId>(parents));
    }
    ClassId create(std::string name, std::vector<ClassId> parents) {
        if (name.empty() || names_.count(name)) throw std::invalid_argument("class name must be unique and nonempty");
        std::set<ClassId> seen;
        for (auto base : parents) {
            get(base);
            if (!seen.insert(base).second) throw std::invalid_argument("duplicate direct base");
        }
        auto id = classes_.size();
        ClassState state;
        state.definition.name = name;
        state.mro = linearize(id, parents);
        state.fields = inherited_fields(parents);
        state.definition.vtable = inherited_methods(parents, state.mro);
        state.revisions.push_back({state.fields.size(), state.definition.vtable.size() != 0});
        if (!parents.empty()) state.definition.parent = parents.front();
        state.definition.parents = std::move(parents);
        classes_.push_back(std::move(state));
        try { names_.emplace(std::move(name), id); } catch (...) { classes_.pop_back(); throw; }
        for (auto base : get(id).parents) classes_[base].definition.frozen = true;
        return id;
    }
    ClassId lookup(const std::string& name) const { return names_.at(name); }
    const ClassDefinition& get(ClassId id) const { return classes_.at(id).definition; }
    const std::vector<ClassId>& linearization(ClassId id) const { return classes_.at(id).mro; }
    bool is_subclass(ClassId derived, ClassId base) const {
        get(base);
        const auto& mro = linearization(derived);
        return std::find(mro.begin(), mro.end(), base) != mro.end();
    }
    ClassLayoutRevision layout_revision(ClassId id) const { return classes_.at(id).revisions.size() - 1; }
    ClassLayoutSchema layout_schema(ClassId id, ClassLayoutRevision revision) const {
        const auto& state = classes_.at(id);
        const auto& shape = state.revisions.at(revision);
        ClassLayoutSchema result{{}, shape.polymorphic};
        result.fields.reserve(shape.field_count);
        for (std::size_t i = 0; i < shape.field_count; ++i) result.fields.push_back(field(state.fields[i]));
        return result;
    }
    std::vector<ClassField> fields(ClassId id) const { return layout_schema(id, layout_revision(id)).fields; }

    void add_field(ClassId id, std::string name, type::Type field_type) {
        require_mutable(id);
        append_field(id, {std::move(name), std::move(field_type)}, {id});
    }

    // Explicitly extend even a frozen class. Append to each affected layout so
    // existing offsets and historical layouts survive, including in diamonds.
    ClassLayoutRevision evolve_field(ClassId id, std::string name, type::Type field_type) {
        get(id);
        std::vector<ClassId> affected{id};
        // Bases always predate their descendants.
        for (auto candidate = id + 1; candidate < classes_.size(); ++candidate)
            if (is_subclass(candidate, id)) affected.push_back(candidate);
        append_field(id, {std::move(name), std::move(field_type)}, affected);
        return layout_revision(id);
    }

    std::size_t define_method(ClassId id, Method method) {
        require_mutable(id);
        validate_method(method);
        auto& state = classes_[id];
        auto& table = state.definition.vtable;
        const bool first = table.size() == 0;
        if (first) state.revisions.reserve(state.revisions.size() + 1);
        auto entry = state.local_methods.insert(method.name);
        try {
            auto slot = table.install(std::move(method));
            if (first) state.revisions.push_back({state.fields.size(), true});
            return slot;
        } catch (...) {
            if (entry.second) state.local_methods.erase(entry.first);
            throw;
        }
    }
    void freeze(ClassId id) { classes_.at(id).definition.frozen = true; }

private:
    using FieldOrigin = std::pair<ClassId, std::size_t>;
    struct Revision { std::size_t field_count; bool polymorphic; };
    struct ClassState {
        ClassDefinition definition;
        std::vector<ClassId> mro;
        std::vector<FieldOrigin> fields;
        std::set<std::string> local_methods;
        std::vector<Revision> revisions;
    };
    std::vector<ClassState> classes_;
    std::map<std::string, ClassId> names_;

    void require_mutable(ClassId id) const { if (get(id).frozen) throw std::logic_error("class is frozen after derivation or finalization"); }
    const ClassField& field(FieldOrigin origin) const { return get(origin.first).fields[origin.second]; }

    // C3 preserves each base's method resolution order and local base precedence.
    std::vector<ClassId> linearize(ClassId id, const std::vector<ClassId>& parents) const {
        std::vector<ClassId> result{id};
        if (parents.empty()) return result;
        if (parents.size() == 1) {
            const auto& base = linearization(parents.front());
            result.insert(result.end(), base.begin(), base.end());
            return result;
        }
        using Iterator = std::vector<ClassId>::const_iterator;
        std::vector<std::pair<Iterator, Iterator>> sequences;
        for (auto base : parents) {
            const auto& order = linearization(base);
            sequences.emplace_back(order.begin(), order.end());
        }
        sequences.emplace_back(parents.begin(), parents.end());
        while (true) {
            std::optional<ClassId> next;
            bool remaining = false;
            for (const auto& sequence : sequences) {
                if (sequence.first == sequence.second) continue;
                remaining = true;
                auto head = *sequence.first;
                bool blocked = false;
                for (const auto& other : sequences)
                    if (other.first != other.second && std::find(other.first + 1, other.second, head) != other.second) {
                        blocked = true; break;
                    }
                if (!blocked) { next = head; break; }
            }
            if (!remaining) return result;
            if (!next) throw std::invalid_argument("inconsistent base precedence");
            result.push_back(*next);
            for (auto& sequence : sequences)
                if (sequence.first != sequence.second && *sequence.first == *next) ++sequence.first;
        }
    }

    std::vector<FieldOrigin> inherited_fields(const std::vector<ClassId>& parents) const {
        if (parents.size() == 1) return classes_[parents.front()].fields;
        std::vector<FieldOrigin> result;
        std::map<std::string, FieldOrigin> seen;
        for (auto base : parents)
            for (auto origin : classes_[base].fields) {
                auto entry = seen.emplace(field(origin).first, origin);
                if (entry.second) result.push_back(origin);
                else if (entry.first->second != origin) throw std::invalid_argument("ambiguous inherited field: " + entry.first->first);
            }
        return result;
    }

    VTable inherited_methods(const std::vector<ClassId>& parents, const std::vector<ClassId>& mro) const {
        if (parents.empty()) return {};
        auto table = get(parents.front()).vtable;
        if (parents.size() == 1) return table;
        // Keep primary slots, append new secondary names, and reject incompatible signatures.
        for (auto base = parents.begin() + 1; base != parents.end(); ++base) {
            const auto& inherited = get(*base).vtable;
            for (std::size_t slot = 0; slot < inherited.size(); ++slot) table.install(inherited.method(slot));
        }
        // Resolve by declaration, not by inherited copies of a shared ancestor's method.
        for (auto i = mro.size(); i > 1; --i) {
            const auto& state = classes_[mro[i - 1]];
            for (const auto& name : state.local_methods) table.install(state.definition.vtable.method(name));
        }
        return table;
    }

    void append_field(ClassId id, ClassField added, const std::vector<ClassId>& affected) {
        type::validate(added.second);
        if (added.first.empty() || added.first == "$vptr") throw std::invalid_argument("invalid class field name");
        for (auto candidate : affected)
            for (auto origin : classes_[candidate].fields)
                if (field(origin).first == added.first) throw std::invalid_argument("duplicate class field: " + added.first);
        auto& own = classes_[id].definition.fields;
        const FieldOrigin origin{id, own.size()};
        own.push_back(std::move(added));
        std::size_t updated = 0;
        try {
            for (auto candidate : affected) {
                auto& state = classes_[candidate];
                state.fields.push_back(origin);
                try { state.revisions.push_back({state.fields.size(), state.revisions.back().polymorphic}); }
                catch (...) { state.fields.pop_back(); throw; }
                ++updated;
            }
        } catch (...) {
            while (updated) {
                auto& state = classes_[affected[--updated]];
                state.fields.pop_back();
                state.revisions.pop_back();
            }
            own.pop_back();
            throw;
        }
    }
};

} // namespace firestone::object
