#include "asdl/schema.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

void require(bool result, std::string_view message)
{
    if (!result) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

asdl::Field field(std::string type, std::string name = {}, char qualifier = 0)
{
    asdl::Field result;
    result.type = std::move(type);
    result.name = std::move(name);
    result.qualifier = qualifier;
    result.span = asdl::SourceMap("memory.asdl", "\n()").span(2, 3);
    return result;
}

asdl::Schema fixture()
{
    asdl::Schema schema;
    schema.file = "memory.asdl";
    schema.module = "Sample";
    schema.generics["array[3]"] = {"array", {"3"}, {}};
    schema.generics["map[string,array[3]]"] = {"map", {"string", "array[3]"}, {}};
    asdl::Definition pair;
    pair.name = "pair";
    pair.fields = {field("int"), field("bool", "int1"), field("int", "true"),
                   field("int", "true_"), field("map[string,array[3]]", "data", '?')};
    asdl::Definition value;
    value.name = "value";
    value.kind = asdl::DefinitionKind::sum;
    value.constructors = {{"Empty", {}, 0, {}}, {"Full", {field("pair")}, 0, {}}};
    value.attributes = {field("value", "kind"), field("int", "v")};
    asdl::Definition flag;
    flag.name = "flag";
    flag.kind = asdl::DefinitionKind::sum;
    flag.constructors = {{"On", {}, 0, {}}, {"Off", {}, 0, {}}};
    schema.definitions = {pair, value, flag};
    return schema;
}

void locations()
{
    asdl::SourceMap map("f", "a\r\nb\n");
    const auto loc = map.span(1, 5);
    require(loc.file == "f", "retained filename");
    require(loc.begin.line == 1 && loc.begin.column == 2, "CRLF begin");
    require(loc.end.line == 3 && loc.end.column == 1, "newline at EOF");
    const asdl::Diagnostic error(loc, "error");
    require(std::string_view(error.what()) == "f:1:2: error", "diagnostic format");
}

void normalizes()
{
    auto schema = fixture();
    asdl::resolve_options(schema, {{"prefix", "Tree_"}, {"omit-builtin", "ASDLHashMap,event,"}});
    asdl::validate(schema);
    require(schema.prefix == "Tree_" && schema.guard == "TREE_AST_H", "namespace defaults");
    require(schema.omitted_builtins.count("map") && schema.omitted_builtins.count("event"),
            "omission aliases");
    require(schema.used_builtins.count("array") && schema.used_builtins.count("map") &&
            schema.used_builtins.count("string"), "nested builtin dependencies");
    const auto& pair = schema.definitions[schema.types.at("pair")];
    require(pair.fields[0].name == "int1_" && pair.fields[0].unnamed, "synthetic name collision");
    require(pair.fields[2].c_name == "true_" && pair.fields[3].c_name == "true__2",
            "reserved macro and post-mangle collision");
    require(asdl::field_type(schema, pair.fields[4]) == "Tree_map_string_array_3_opt",
            "generic C name");
    require(schema.type_names.at(pair.fields[4].type_id) == pair.fields[4].type, "intern id");
    require(schema.definitions[1].attributes[0].c_name == "kind_", "attribute reservation");
    require(schema.definitions[1].attributes[1].c_name == "v_", "union reservation");
    const auto off = schema.constructors.at("Off");
    require(schema.definitions[off.definition].is_enum, "enum inferred");
    require(schema.definitions[off.definition].constructors[off.constructor].tag == 2, "tag order");

    auto copy = schema;
    schema = {};
    asdl::validate(copy);
    require(copy.type_names.at(copy.definitions[0].fields[4].type_id) ==
            "map[string,array[3]]", "model independent after copy");
    require(copy.definitions[0].fields[0].name == "int1_", "idempotent synthetic names");
}

void failures()
{
    auto schema = fixture();
    schema.definitions[0].fields.push_back(field("value"));
    try {
        asdl::validate(schema);
        require(false, "product -> sum -> product must fail");
    } catch (const asdl::Diagnostic& error) {
        require(error.message == "recursive product type: pair -> value -> pair",
                "cycle diagnostic");
        require(error.span.begin.line == 2 && error.span.begin.column == 2, "cycle source");
    }
    schema = fixture();
    schema.generics.at("array[3]").args = {"never_defined", "3"};
    try {
        asdl::validate(schema);
        require(false, "undefined nested type must fail");
    } catch (const asdl::Diagnostic& error) {
        require(error.message == "undefined type 'never_defined'", "nested type diagnostic");
    }
    schema = fixture();
    schema.definitions[0].fields.push_back(field("int", "data"));
    try {
        asdl::validate(schema);
        require(false, "duplicate field must fail");
    } catch (const asdl::Diagnostic& error) {
        require(error.message == "duplicate field 'data'", "duplicate diagnostic");
    }
}

} // namespace

int main()
{
    locations();
    normalizes();
    failures();
}
