#include "asdl/ASDL.h"
#include "asdl/dot.hpp"
#include "asdl/schema.hpp"

#include <filesystem>
#include <ios>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

struct asdl_schema {
    asdl::Schema value;
};

struct asdl_buffer {
    std::string value;
};

struct asdl_error {
    std::string message;
    std::string source;
    std::size_t line;
    std::size_t column;
};

namespace {

asdl_status fail(asdl_error **out, asdl_status status, std::string_view message,
                 std::string_view source = {}, std::size_t line = 0,
                 std::size_t column = 0) noexcept
{
    if (out) {
        try {
            *out = new asdl_error{std::string(message), std::string(source), line, column};
        } catch (...) {
            // Diagnostics are best effort, including when the original failure
            // was not caused by allocation.
            *out = nullptr;
        }
    }
    return status;
}

template <typename Handle, typename Action>
asdl_status produce(Handle **result, asdl_error **error, Action action) noexcept
{
    if (error) *error = nullptr;
    if (!result) return fail(error, ASDL_INVALID_ARGUMENT, "missing result slot");
    *result = nullptr;
    try {
        return action();
    } catch (const asdl::Diagnostic &exc) {
        return fail(error, ASDL_SCHEMA_ERROR, exc.message, exc.span.file,
                    exc.span.begin.line, exc.span.begin.column);
    } catch (const std::bad_alloc &) {
        return fail(error, ASDL_OUT_OF_MEMORY, "out of memory");
    } catch (const std::filesystem::filesystem_error &exc) {
        return fail(error, ASDL_IO_ERROR, exc.what());
    } catch (const std::ios_base::failure &exc) {
        return fail(error, ASDL_IO_ERROR, exc.what());
    } catch (const std::exception &exc) {
        return fail(error, ASDL_INTERNAL_ERROR, exc.what());
    } catch (...) {
        return fail(error, ASDL_INTERNAL_ERROR, "unknown internal failure");
    }
}

} // namespace

extern "C" {

uint32_t asdl_abi_version(void) { return ASDL_ABI_VERSION; }

asdl_status asdl_parse(const char *source, size_t length, const char *source_name,
                       asdl_schema **out_schema, asdl_error **out_error)
{
    return produce(out_schema, out_error, [&]() -> asdl_status {
        if (!source && length) return fail(out_error, ASDL_INVALID_ARGUMENT, "missing source bytes");
        auto schema = asdl::parse(std::string_view(source ? source : "", length),
                                  source_name ? source_name : "<input>");
        asdl::resolve(schema);
        asdl::validate(schema);
        *out_schema = new asdl_schema{std::move(schema)};
        return ASDL_OK;
    });
}

asdl_status asdl_load(const char *path, asdl_schema **out_schema, asdl_error **out_error)
{
    return produce(out_schema, out_error, [&]() -> asdl_status {
        if (!path || !*path) return fail(out_error, ASDL_INVALID_ARGUMENT, "missing input path");
        asdl::Schema schema;
        try {
            schema = asdl::parse_file(path);
        } catch (const std::system_error &exc) {
            return fail(out_error, ASDL_IO_ERROR, exc.what(), path);
        }
        asdl::validate(schema);
        *out_schema = new asdl_schema{std::move(schema)};
        return ASDL_OK;
    });
}

void asdl_schema_destroy(asdl_schema *schema) { delete schema; }

const char *asdl_schema_module(const asdl_schema *schema)
{
    return schema ? schema->value.module.c_str() : nullptr;
}

size_t asdl_schema_type_count(const asdl_schema *schema)
{
    return schema ? schema->value.definitions.size() : 0;
}

const char *asdl_schema_type_name(const asdl_schema *schema, size_t index)
{
    return index < asdl_schema_type_count(schema)
        ? schema->value.definitions[index].name.c_str()
        : nullptr;
}

asdl_status asdl_schema_dot(const asdl_schema *schema, asdl_dot_view view,
                            asdl_buffer **out_buffer, asdl_error **out_error)
{
    return produce(out_buffer, out_error, [&]() -> asdl_status {
        if (!schema) return fail(out_error, ASDL_INVALID_ARGUMENT, "missing schema");
        asdl::DotView cpp_view;
        switch (view) {
        case ASDL_DOT_SCHEMA: cpp_view = asdl::DotView::schema; break;
        case ASDL_DOT_DEPENDENCIES: cpp_view = asdl::DotView::dependencies; break;
        case ASDL_DOT_IMPORTS: cpp_view = asdl::DotView::imports; break;
        default: return fail(out_error, ASDL_INVALID_ARGUMENT, "invalid DOT view");
        }
        *out_buffer = new asdl_buffer{asdl::emit_dot(schema->value, cpp_view)};
        return ASDL_OK;
    });
}

const char *asdl_buffer_data(const asdl_buffer *buffer)
{
    return buffer ? buffer->value.c_str() : nullptr;
}

size_t asdl_buffer_size(const asdl_buffer *buffer)
{
    return buffer ? buffer->value.size() : 0;
}

void asdl_buffer_destroy(asdl_buffer *buffer) { delete buffer; }

const char *asdl_error_message(const asdl_error *error)
{
    return error ? error->message.c_str() : nullptr;
}

const char *asdl_error_source(const asdl_error *error)
{
    return error && !error->source.empty() ? error->source.c_str() : nullptr;
}

size_t asdl_error_line(const asdl_error *error) { return error ? error->line : 0; }

size_t asdl_error_column(const asdl_error *error) { return error ? error->column : 0; }

void asdl_error_destroy(asdl_error *error) { delete error; }

} // extern "C"
