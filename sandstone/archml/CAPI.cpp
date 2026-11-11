#include "ArchML.h"
#include "Target.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace sandstone::archml;
struct sandstone_archml_document { Document value; };
namespace {
void clear(sandstone_archml_error *error) { if (error) *error = {}; }
void report(sandstone_archml_error *error, const char *message, Location at = {}) noexcept {
    if (!error) return;
    error->offset = at.offset; error->line = at.line; error->column = at.column;
    std::snprintf(error->message, sizeof(error->message), "%s", message);
}
template<class F> sandstone_archml_status guarded(sandstone_archml_error *error, F action) noexcept {
    clear(error);
    try { return action(); }
    catch (const Error &e) { report(error, e.what(), e.location); }
    catch (const std::exception &e) { report(error, e.what()); }
    catch (...) { report(error, "unknown ArchML error"); }
    return SANDSTONE_ARCHML_INVALID;
}
void require(bool condition) { if (!condition) throw std::invalid_argument("invalid null argument"); }
void copy(const std::string &text, char **output, size_t *length) {
    auto *memory = static_cast<char *>(std::malloc(text.size() + 1));
    if (!memory) throw std::bad_alloc();
    std::memcpy(memory, text.c_str(), text.size() + 1);
    *output = memory; *length = text.size();
}
}
extern "C" {
sandstone_archml_document *sandstone_archml_parse(const char *text, size_t length, sandstone_archml_error *error) {
    sandstone_archml_document *result = nullptr;
    guarded(error, [&] {
        require(text || !length);
        auto doc = Document::parse(std::string_view(text ? text : "", length));
        result = new sandstone_archml_document{std::move(doc)};
        return SANDSTONE_ARCHML_OK;
    });
    return result;
}
sandstone_archml_document *sandstone_archml_load(const char *path, sandstone_archml_error *error) {
    sandstone_archml_document *result = nullptr;
    guarded(error, [&] {
        require(path);
        auto doc = Document::load(path);
        result = new sandstone_archml_document{std::move(doc)};
        return SANDSTONE_ARCHML_OK;
    });
    return result;
}
void sandstone_archml_document_free(sandstone_archml_document *doc) { delete doc; }
void sandstone_archml_free(void *memory) { std::free(memory); }
const char *sandstone_archml_name(const sandstone_archml_document *doc) { return doc ? doc->value.name().c_str() : nullptr; }
size_t sandstone_archml_instruction_count(const sandstone_archml_document *doc) { return doc ? doc->value.instructions().size() : 0; }
const char *sandstone_archml_instruction_name(const sandstone_archml_document *doc, size_t index) {
    return doc && index < doc->value.instructions().size() ? doc->value.instructions()[index].name.c_str() : nullptr;
}
sandstone_archml_status sandstone_archml_generate(const sandstone_archml_document *doc, const char *target,
    const char *ns, char **output, size_t *length, sandstone_archml_error *error) {
    return guarded(error, [&] {
        require(doc && target && output && length);
        copy(generate(doc->value, target, {ns ? ns : ""}), output, length);
        return SANDSTONE_ARCHML_OK;
    });
}
sandstone_archml_status sandstone_archml_find(const sandstone_archml_document *doc, const char *kind,
    const char *name, char **output, size_t *length, sandstone_archml_error *error) {
    return guarded(error, [&] {
        require(doc && kind && name && output && length);
        const auto *node = doc->value.find(kind, name);
        if (!node) return SANDSTONE_ARCHML_NO_MATCH;
        copy(json(*node), output, length);
        return SANDSTONE_ARCHML_OK;
    });
}
sandstone_archml_status sandstone_archml_encode(const sandstone_archml_document *doc, const char *name,
    const uint64_t *operands, size_t count, uint8_t *output, size_t *length, sandstone_archml_error *error) {
    return guarded(error, [&] {
        require(doc && name && length && (operands || !count));
        auto bytes = doc->value.encode(name, {operands, count});
        if (!output || *length < bytes.size()) { *length = bytes.size(); return SANDSTONE_ARCHML_BUFFER_TOO_SMALL; }
        std::memcpy(output, bytes.data(), bytes.size()); *length = bytes.size();
        return SANDSTONE_ARCHML_OK;
    });
}
sandstone_archml_status sandstone_archml_decode(const sandstone_archml_document *doc, const uint8_t *bytes,
    size_t length, size_t *index, uint64_t *operands, size_t *count, size_t *consumed, sandstone_archml_error *error) {
    return guarded(error, [&] {
        require(doc && (bytes || !length) && index && count && consumed);
        runtime::Decoded result;
        if (!doc->value.decode({bytes, length}, result)) return SANDSTONE_ARCHML_NO_MATCH;
        if (*count < result.operands.size() || (!operands && !result.operands.empty())) {
            *count = result.operands.size(); return SANDSTONE_ARCHML_BUFFER_TOO_SMALL;
        }
        if (!result.operands.empty()) std::memcpy(operands, result.operands.data(), result.operands.size() * sizeof(*operands));
        *count = result.operands.size(); *index = result.instruction; *consumed = result.size;
        return SANDSTONE_ARCHML_OK;
    });
}
}
