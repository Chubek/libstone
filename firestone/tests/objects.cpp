#include "Test.hpp"

using namespace firestone::object;
using firestone::ir::Type;

static auto integer_field(unsigned bits = 32) {
    return firestone::type::Type::atomic(Type::integer(bits));
}

static void multiple_inheritance() {
    ClassHierarchy h;
    auto root = h.create("Root");
    h.add_field(root, "shared", integer_field(8));
    auto slot = h.define_method(root, {"method", "Root.method", Type::integer(), {}});
    auto left = h.create("Left", root), right = h.create("Right", root);
    h.add_field(left, "left", integer_field());
    h.add_field(right, "right", integer_field());
    h.define_method(left, {"pick", "Left.pick", Type::void_type(), {}});
    auto local_slot = h.define_method(left, {"local", "Left.local", Type::void_type(), {}});
    h.define_method(right, {"pick", "Right.pick", Type::void_type(), {}});
    CHECK(h.define_method(right, {"method", "Right.method", Type::integer(), {}}) == slot);
    CHECK(h.define_method(right, {"extra", "Right.extra", Type::void_type(), {}}) == local_slot);
    auto diamond = h.create("Diamond", {left, right});
    CHECK(h.get(diamond).parent == left && h.get(diamond).parents == std::vector<ClassId>{left, right});
    CHECK(h.linearization(diamond) == std::vector<ClassId>{diamond, left, right, root});
    CHECK(h.is_subclass(diamond, root) && h.is_subclass(diamond, right) && h.is_subclass(diamond, diamond));
    CHECK(!h.is_subclass(left, right));
    CHECK(h.get(left).frozen && h.get(right).frozen);
    CHECK(h.fields(diamond) == std::vector<ClassField>{
        {"shared", integer_field(8)}, {"left", integer_field()}, {"right", integer_field()}});
    auto layout = object_layout(h, diamond);
    CHECK(layout.vptr_offset == 0 && layout.offsets.at("shared") == 8);
    CHECK(layout.offsets.at("left") == 12 && layout.offsets.at("right") == 16);
    auto target = dispatch(h, diamond, "method", {});
    CHECK(target.slot == slot && target.method.target == "Right.method");
    CHECK(dispatch(h, diamond, "pick", {}).method.target == "Left.pick");
    CHECK(dispatch(h, diamond, "local", {}).slot == local_slot);
    auto extra = dispatch(h, diamond, "extra", {});
    CHECK(extra.slot == local_slot + 1 && extra.method.target == "Right.extra");
    CHECK(h.define_method(diamond, {"method", "Diamond.method", Type::integer(), {}}) == slot);
    CHECK(dispatch(h, diamond, "method", {}).method.target == "Diamond.method");

    auto backwards = h.create("Backwards", std::vector<ClassId>{right, left});
    CHECK(dispatch(h, backwards, "pick", {}).method.target == "Right.pick");
    throws<std::invalid_argument>([&] { h.create("BadOrder", {diamond, backwards}); });
    CHECK(!h.get(diamond).frozen && !h.get(backwards).frozen);
    throws<std::out_of_range>([&] { h.lookup("BadOrder"); });

    auto x = h.create("X", {}), y = h.create("Y");
    throws<std::invalid_argument>([&] { h.create("DuplicateBase", {x, x}); });
    throws<std::out_of_range>([&] { h.create("UnknownBase", {x, 1000}); });
    h.add_field(x, "collision", integer_field());
    h.add_field(y, "collision", integer_field());
    throws<std::invalid_argument>([&] { h.create("BadField", {x, y}); });
    auto z = h.create("Z");
    h.define_method(x, {"f", "X.f", Type::integer(), {}});
    h.define_method(z, {"f", "Z.f", Type::floating(), {}});
    throws<std::invalid_argument>([&] { h.create("BadMethod", {x, z}); });
    CHECK(!h.get(x).frozen && !h.get(y).frozen && !h.get(z).frozen);
}

static void layout_evolution() {
    ClassHierarchy h;
    auto base = h.create("Base");
    CHECK(h.layout_revision(base) == 0);
    h.add_field(base, "base", integer_field(8));
    auto nonpoly = h.layout_revision(base);
    h.define_method(base, {"call", "Base.call", Type::void_type(), {}});
    CHECK(h.layout_revision(base) == nonpoly + 1);
    CHECK(!object_layout_at(h, base, nonpoly).vptr_offset);
    CHECK(object_layout_at(h, base, nonpoly).offsets.at("base") == 0);
    auto left = h.create("Left", base), right = h.create("Right", base);
    h.add_field(left, "left", integer_field(8));
    h.add_field(right, "right", integer_field());
    auto diamond = h.create("Diamond", {left, right});
    h.add_field(diamond, "own", integer_field());
    h.freeze(diamond);
    auto unrelated = h.create("Unrelated");

    auto base_before = h.layout_revision(base), left_before = h.layout_revision(left);
    auto right_before = h.layout_revision(right), diamond_before = h.layout_revision(diamond);
    auto old_layout = object_layout(h, diamond);
    throws<std::logic_error>([&] { h.add_field(base, "late", integer_field(64)); });
    throws<std::logic_error>([&] { h.add_field(diamond, "late", integer_field(64)); });
    // Check all descendants before changing any layout, even if only the diamond conflicts.
    throws<std::invalid_argument>([&] { h.evolve_field(base, "own", integer_field()); });
    CHECK(h.layout_revision(base) == base_before && h.layout_revision(left) == left_before);
    CHECK(h.layout_revision(right) == right_before && h.layout_revision(diamond) == diamond_before);
    CHECK(h.evolve_field(base, "late", integer_field(64)) == base_before + 1);
    CHECK(h.layout_revision(left) == left_before + 1 && h.layout_revision(right) == right_before + 1);
    CHECK(h.layout_revision(diamond) == diamond_before + 1 && h.layout_revision(unrelated) == 0);
    CHECK(h.fields(diamond).size() == 5 && h.get(diamond).fields.size() == 1 && h.get(diamond).frozen);
    CHECK(h.fields(diamond).back() == ClassField{"late", integer_field(64)});
    auto updated = object_layout(h, diamond);
    CHECK(updated.vptr_offset == old_layout.vptr_offset && updated.size > old_layout.size);
    for (const auto& [name, offset] : old_layout.offsets) CHECK(updated.offsets.at(name) == offset);
    auto historical = object_layout_at(h, diamond, diamond_before);
    CHECK(historical.offsets == old_layout.offsets && historical.size == old_layout.size);
    CHECK(object_layout_at(h, base, nonpoly).offsets.count("late") == 0);
    CHECK(dispatch(h, diamond, "call", {}).method.target == "Base.call");
    throws<std::out_of_range>([&] { object_layout_at(h, diamond, diamond_before + 2); });

    auto future = h.create("Future", diamond);
    CHECK(object_layout(h, future).offsets == updated.offsets);
    CHECK(h.evolve_field(diamond, "own_late", integer_field()) == diamond_before + 2);
    CHECK(h.layout_revision(future) == 1 && object_layout(h, future).offsets.count("own_late") == 1);
}

int main() { return test_main([] {
    ClassHierarchy hierarchy;
    auto base = hierarchy.create("Base");
    hierarchy.add_field(base, "tag", firestone::type::Type::atomic(Type::integer(8)));
    auto slot = hierarchy.define_method(base, {"compute", "Base.compute", Type::integer(), {Type::integer()}});
    auto derived = hierarchy.create("Derived", base);
    CHECK(hierarchy.is_subclass(derived, base) && !hierarchy.is_subclass(base, derived));
    throws<std::logic_error>([&] { hierarchy.add_field(base, "later", firestone::type::Type::atomic(Type::integer())); });
    CHECK(hierarchy.define_method(derived, {"compute", "Derived.compute", Type::integer(), {Type::integer()}}) == slot);
    throws<std::invalid_argument>([&] { hierarchy.define_method(derived, {"compute", "bad", Type::floating(), {Type::integer()}}); });
    CHECK(hierarchy.define_method(derived, {"other", "Derived.other", Type::void_type(), {}}) == slot + 1);
    CHECK(hierarchy.get(derived).vtable.method(slot).target == "Derived.compute");
    throws<std::invalid_argument>([&] { hierarchy.add_field(derived, "tag", firestone::type::Type::atomic(Type::integer())); });
    hierarchy.add_field(derived, "number", firestone::type::Type::atomic(Type::integer()));
    auto layout = object_layout(hierarchy, derived);
    CHECK(layout.vptr_offset == 0 && layout.offsets.at("tag") == 8 && layout.offsets.at("number") == 12 && layout.size == 16);
    CHECK(object_layout(hierarchy, derived, {4, 4, 4}).size == 12);

    ObjectGraph objects;
    auto a = objects.create("Derived"), b = objects.create("Base");
    objects.set(a, "next", ObjectRef{b}); objects.set(a, "also", ObjectRef{b}); objects.set(b, "next", ObjectRef{a});
    objects.set(a, "number", std::int64_t{42});
    CHECK(objects.reachable(a).size() == 2);
    auto target = dispatch(hierarchy, objects, a, "compute", {Type::integer()});
    CHECK(target.slot == slot && target.method.target == "Derived.compute");
    throws<std::invalid_argument>([&] { dispatch(hierarchy, objects, a, "compute", {Type::floating()}); });
    throws<std::logic_error>([&] { objects.remove(b); });
    auto cloned = clone_reachable(objects, a, objects);
    CHECK(cloned.root != a && objects.reachable(cloned.root).size() == 2);
    CHECK(std::get<ObjectRef>(objects.field(cloned.root, "next")).id == cloned.mapping.at(b));
    CHECK(std::get<ObjectRef>(objects.field(cloned.mapping.at(b), "next")).id == cloned.root);
    CHECK(std::get<ObjectRef>(objects.field(cloned.root, "next")) == std::get<ObjectRef>(objects.field(cloned.root, "also")));
    std::size_t visits = 0; visit_reachable(objects, cloned.root, [&](ObjectId, const Object&) { ++visits; }); CHECK(visits == 2);
    objects.erase_field(a, "next"); CHECK(objects.reachable(a).size() == 2);
    objects.erase_field(a, "also"); objects.remove(b); CHECK(!objects.contains(b));
    throws<std::out_of_range>([&] { objects.set(a, "dangling", ObjectRef{b}); });
    CHECK(objects.get(a).fields.count("dangling") == 0);
    multiple_inheritance();
    layout_evolution();
}); }
