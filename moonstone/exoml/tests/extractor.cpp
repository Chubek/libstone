#include <cassert>
#include <exoml/Extract.hpp>
#include <iostream>

using namespace moonstone::exoml::cdecl;
const Declaration &decl(const TranslationUnit &u, std::string_view name) {
    for (const auto &d : u.declarations)
        if (d.name == name)
            return d;
    throw std::runtime_error("missing declaration: " + std::string(name));
}
const Type &underlying(const TranslationUnit &u, const std::string &id) {
    const Type *type = &u.types.at(id);
    while (type->kind == TypeKind::Typedef || type->kind == TypeKind::Elaborated)
        type = &u.types.at(type->element);
    return *type;
}
int main() {
    ExtractOptions o;
    o.header = EXOML_DISCOVERY_FIXTURE;
    o.resource_directory = EXOML_CLANG_RESOURCES;
    o.revision = "fixture-revision";
    auto u = extract_c11(o);
    for (const auto &d : u.diagnostics)
        std::cerr << moonstone::format(d) << '\n';
    assert(u.complete && u.fingerprint.size() == 64 && !u.inputs.empty());
    assert(u.target.pointer_bits > 0 && !u.target.compiler.empty() && !u.target.triple.empty());
    assert(describe_translation_unit(u) == describe_translation_unit(extract_c11(o)));
    const auto &alias = u.types.at(decl(u, "exoml_alias").type);
    assert(alias.kind == TypeKind::Typedef);
    assert(u.types.at(alias.element).kind == TypeKind::Typedef);
    assert(underlying(u, alias.id).is_signed == false);
    const auto &callback = underlying(u, decl(u, "exoml_callback").type);
    assert(callback.kind == TypeKind::Pointer);
    const auto &signature = u.types.at(callback.element);
    assert(signature.kind == TypeKind::Function && signature.arguments.size() == 3);
    assert(signature.prototype && !signature.variadic);
    assert(decl(u, "exoml_call").parameters[1].name == "callback");
    const auto &node = underlying(u, decl(u, "exoml_node").type);
    assert(node.kind == TypeKind::Record && node.fields.size() == 5);
    const auto &recursive = underlying(u, u.types.at(node.fields[0].type).element);
    assert(recursive.fields.size() == node.fields.size());
    const auto &pointer = u.types.at(node.fields[1].type);
    const auto &pointee = u.types.at(pointer.element);
    assert(pointee.qualifiers.is_const && pointee.qualifiers.is_volatile);
    assert(u.types.at(node.fields[2].type).array_count == 7);
    assert(node.fields[3].bit_width == 3);
    assert(underlying(u, node.fields[4].type).is_union);
    const auto &anonymous = underlying(u, decl(u, "exoml_anonymous").type);
    assert(anonymous.fields.size() == 2 && anonymous.fields[0].name.empty());
    assert(underlying(u, anonymous.fields[0].type).is_union);
    assert(!u.types.at(anonymous.fields[1].type).array_count);
    const auto &forward_pointer = underlying(u, decl(u, "exoml_forward_pointer").type);
    assert(underlying(u, forward_pointer.element).fields.size() == 1);
    const auto &const_pointer = u.types.at(decl(u, "exoml_const_node").parameters[0].type);
    assert(u.types.at(const_pointer.element).qualifiers.is_const);
    assert(!underlying(u, decl(u, "exoml_opaque").type).size);
    assert(underlying(u, decl(u, "exoml_packed").type).alignment == 1);
    assert(underlying(u, decl(u, "exoml_aligned").type).alignment == 32);
    assert(decl(u, "exoml_inline").is_inline && decl(u, "exoml_inline").definition);
    assert(u.types.at(decl(u, "exoml_variadic").type).variadic);
    assert(!u.types.at(decl(u, "exoml_unprototyped").type).prototype);
    assert(decl(u, "EXOML_INCREMENT").function_like_macro);
    assert(!decl(u, "EXOML_NUMBER").function_like_macro);
    const auto &array = decl(u, "exoml_array");
    assert(u.types.at(array.parameters[0].type).kind == TypeKind::Array);
    assert(u.types.at(array.parameters[1].type).qualifiers.is_restrict);
    assert(decl(u, "exoml_call").source.file == u.header);
    assert(decl(u, "exoml_call").source.line > 1);
    for (const auto &[id, t] : u.types) {
        assert(id == t.id && u.types.contains(t.canonical_id));
        if (!t.element.empty())
            assert(u.types.contains(t.element));
        if (!t.result.empty())
            assert(u.types.contains(t.result));
        for (const auto &a : t.arguments)
            assert(u.types.contains(a));
    }
    auto changed = o;
    changed.defines = {"EXOML_ALTERNATE=1"};
    auto alternate = extract_c11(changed);
    assert(alternate.complete && alternate.fingerprint != u.fingerprint);
    assert(decl(alternate, "exoml_conditional").kind == DeclKind::Function);
    changed.defines = {"EXOML_INVALID"};
    auto invalid = extract_c11(changed);
    assert(!invalid.complete && !invalid.diagnostics.empty());
    assert(std::any_of(invalid.diagnostics.begin(), invalid.diagnostics.end(), [&](const auto &d) {
        return d.id == "EXOML-C11-COMPILER" && d.primary.file == u.header && d.primary.line > 1;
    }));
    assert(decl(invalid, "exoml_call").kind == DeclKind::Function);
    changed.defines = {"MULIB_IMPL_VEC"};
    assert(!extract_c11(changed).complete);
    changed.defines = {"EXOML_IMPLEMENTATION"};
    assert(!extract_c11(changed).complete);
    changed.defines = {"BAD\n#define INJECTED"};
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.target = "not-a-real-target";
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.limits.types = 0;
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.limits.declarations = 0;
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.limits.depth = 0;
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.limits.input_bytes = 1;
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.limits.entries = 0;
    assert(!extract_c11(changed).complete);
    changed = o;
    changed.header += ".missing";
    assert(!extract_c11(changed).complete);
}
