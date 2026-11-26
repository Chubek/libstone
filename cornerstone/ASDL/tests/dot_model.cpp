#include <asdl/dot.hpp>
#include <asdl/schema.hpp>

#include <iostream>
#include <string>
#include <string_view>

namespace {

bool contains(std::string_view text, std::string_view needle)
{
    if (text.find(needle) != std::string_view::npos) return true;
    std::cerr << "missing DOT fragment: " << needle << '\n';
    return false;
}

} // namespace

int main()
{
    asdl::Schema schema;
    schema.language = "quotes \" and slash \\N\nwith controls\t\r\001";

    asdl::Definition leaf;
    leaf.name = "leaf";
    schema.definitions.push_back(leaf);

    schema.generics.emplace("map_leaf_leaf", asdl::Generic{"map", {"leaf", "leaf"}, {}});
    asdl::Field entries;
    entries.name = "entries";
    entries.type = "map_leaf_leaf";
    entries.refinement = ":entries";
    entries.qualifier = '?';
    asdl::Definition container;
    container.name = "container";
    container.fields.push_back(entries);
    schema.definitions.push_back(container);

    asdl::Import import;
    import.module = "mod\"; injected -> bogus; //";
    import.names.push_back({"Value", "alias\"!", {}});
    schema.imports.push_back(import);
    import.names = {{"Other", "", {}}};
    schema.imports.push_back(import);
    asdl::validate(schema);

    const auto structure = asdl::emit_dot(schema);
    const auto dependencies = asdl::emit_dot(schema, asdl::DotView::dependencies);
    const auto imports = asdl::emit_dot(schema, asdl::DotView::imports);
    const std::string dependency =
        "type_1 -> type_0 [label=\"map_leaf_leaf<:entries>? entries\"]";
    if (!contains(structure, "type_1_field_0 -> type_0 [style=dashed]") ||
        !contains(dependencies, dependency) ||
        dependencies.find(dependency) != dependencies.rfind(dependency) ||
        !contains(imports, R"(label="quotes \" and slash \\N\nwith controls\\t\\r\\x01")") ||
        !contains(imports, R"DOT(import_0 [shape=component, label="mod\"; injected -> bogus; // (Lua)"])DOT") ||
        !contains(imports, R"(root -> import_0 [label="Value as alias\"!"])") ||
        !contains(imports, R"(root -> import_0 [label="Other"])") ||
        imports.find("import_1") != std::string::npos)
        return 1;
    return 0;
}
