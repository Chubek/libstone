/* A C client: no C++ headers, no implementation details, and no Lua state. */
#include <asdl/ASDL.h>

#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

static int includes(const asdl_buffer *buffer, const char *text)
{
    const char *data = asdl_buffer_data(buffer);
    return data && strstr(data, text) != NULL;
}

int main(int argc, char **argv)
{
    const char source[] =
        "module Graph {\n"
        "  span = (int start, int end)\n"
        "  expr = Number(int value) | Combine(expr? left, expr* right, expr+ rest)\n"
        "       attributes (span? location)\n"
        "}\n"
        "IGNORE"; /* Not part of the length passed to the library. */
    const char extended[] =
        ".language \"ABI Graph\";\n"
        ".import JSON, SExpr from std.serialize;\n"
        "value ::= Leaf;\n";
    const char invalid[] = "module Broken {\n  node = (int x,\n";
    const char unknown[] = "module Broken { node = (unknown value) }";
    asdl_schema *schema = NULL;
    asdl_error *error = NULL;
    asdl_buffer *graph = NULL;
    asdl_buffer *copy = NULL;
    asdl_buffer *deps = NULL;
    asdl_buffer *imports = NULL;

    CHECK((asdl_abi_version() >> 16) == (ASDL_ABI_VERSION >> 16));
    CHECK(asdl_parse(source, sizeof(source) - sizeof("IGNORE"), "memory.asdl",
                     &schema, &error) == ASDL_OK);
    CHECK(schema && !error);
    CHECK(strcmp(asdl_schema_module(schema), "Graph") == 0);
    CHECK(asdl_schema_type_count(schema) == 2);
    CHECK(strcmp(asdl_schema_type_name(schema, 0), "span") == 0);
    CHECK(strcmp(asdl_schema_type_name(schema, 1), "expr") == 0);
    CHECK(!asdl_schema_type_name(schema, 2));

    CHECK(asdl_schema_dot(schema, ASDL_DOT_SCHEMA, &graph, &error) == ASDL_OK);
    CHECK(!error && asdl_buffer_size(graph) == strlen(asdl_buffer_data(graph)));
    CHECK(includes(graph, "span (product)") && includes(graph, "expr (sum)"));
    CHECK(includes(graph, "Combine") && includes(graph, "Number"));
    CHECK(includes(graph, "expr? left") && includes(graph, "expr* right"));
    CHECK(includes(graph, "expr+ rest") && includes(graph, "span? location"));
    CHECK(includes(graph, "type_1_attributes_field_0 -> type_0 [style=dashed]"));

    CHECK(asdl_schema_dot(schema, ASDL_DOT_SCHEMA, &copy, NULL) == ASDL_OK);
    CHECK(asdl_buffer_size(graph) == asdl_buffer_size(copy));
    CHECK(memcmp(asdl_buffer_data(graph), asdl_buffer_data(copy),
                 asdl_buffer_size(graph)) == 0);
    CHECK(asdl_schema_dot(schema, ASDL_DOT_DEPENDENCIES, &deps, NULL) == ASDL_OK);
    CHECK(includes(deps, "type_1 -> type_1 [label=\"Combine: expr? left\"]"));
    CHECK(includes(deps, "type_1 -> type_0 [label=\"attributes: span? location\"]"));
    CHECK(!includes(deps, "type_1_ctor_"));
    asdl_buffer_destroy(copy);
    asdl_buffer_destroy(deps);
    copy = NULL;
    CHECK(asdl_schema_dot(schema, UINT32_MAX, &copy, &error) == ASDL_INVALID_ARGUMENT);
    CHECK(!copy && error && asdl_error_message(error));
    asdl_error_destroy(error);
    asdl_schema_destroy(schema);
    /* The text remains valid after destroying its originating schema. */
    CHECK(includes(graph, "digraph ASDL"));
    asdl_buffer_destroy(graph);
    schema = NULL;
    error = NULL;

    CHECK(asdl_parse(extended, sizeof(extended) - 1, NULL, &schema, &error) == ASDL_OK);
    CHECK(!error);
    CHECK(asdl_schema_dot(schema, ASDL_DOT_IMPORTS, &imports, NULL) == ASDL_OK);
    CHECK(includes(imports, "std.serialize (Lua)"));
    CHECK(includes(imports, "JSON, SExpr"));
    CHECK(!includes(imports, "Leaf"));
    asdl_schema_destroy(schema);
    asdl_buffer_destroy(imports);
    schema = NULL;

    CHECK(asdl_parse(invalid, sizeof(invalid) - 1, "broken.asdl", &schema, &error)
          == ASDL_SCHEMA_ERROR);
    CHECK(!schema && error && strcmp(asdl_error_source(error), "broken.asdl") == 0);
    CHECK(asdl_error_line(error) >= 2 && asdl_error_column(error) >= 1);
    CHECK(asdl_error_message(error) && *asdl_error_message(error));
    asdl_error_destroy(error);
    error = NULL;
    CHECK(asdl_parse(unknown, sizeof(unknown) - 1, NULL, &schema, NULL) == ASDL_SCHEMA_ERROR);
    CHECK(!schema);
    CHECK(asdl_parse(NULL, 1, NULL, &schema, NULL) == ASDL_INVALID_ARGUMENT);
    CHECK(asdl_load(NULL, &schema, NULL) == ASDL_INVALID_ARGUMENT);
    CHECK(asdl_schema_dot(NULL, ASDL_DOT_SCHEMA, NULL, NULL) == ASDL_INVALID_ARGUMENT);

    if (argc > 1) {
        CHECK(asdl_load(argv[1], &schema, &error) == ASDL_OK);
        CHECK(!error && schema);
        CHECK(asdl_schema_type_count(schema) != 0);
        asdl_schema_destroy(schema);
    }
    if (argc > 2) {
        CHECK(asdl_load(argv[2], &schema, &error) == ASDL_IO_ERROR);
        CHECK(!schema && error && strcmp(asdl_error_source(error), argv[2]) == 0);
        asdl_error_destroy(error);
    }
    if (argc > 3) {
        CHECK(asdl_load(argv[3], &schema, NULL) == ASDL_OK);
        CHECK(asdl_schema_dot(schema, ASDL_DOT_IMPORTS, &imports, NULL) == ASDL_OK);
        CHECK(includes(imports, "mid (schema)") && includes(imports, "leaf (schema)"));
        CHECK(includes(imports, "root -> import_0 [label=\"as Middle\"]"));
        CHECK(includes(imports, "import_0 -> import_1 [label=\"item as entry\"]"));
        CHECK(includes(imports, "import_0 -> import_2 [label=\"JSON\"]"));
        CHECK(!includes(imports, "root -> import_1") && !includes(imports, "root -> import_2"));
        asdl_buffer_destroy(imports);
        asdl_schema_destroy(schema);
    }
    asdl_schema_destroy(NULL);
    asdl_buffer_destroy(NULL);
    asdl_error_destroy(NULL);
    return 0;
}
