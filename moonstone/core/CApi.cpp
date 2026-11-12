#include "SourceManager.hpp"
#include "moonstone.h"
#include <new>

struct ms_v1_source {
    moonstone::SourceManager manager;
    moonstone::SourceId id = moonstone::invalid_source;
    explicit ms_v1_source(std::size_t max_bytes) : manager({1, max_bytes}) {}
};

namespace {
ms_v1_status status(moonstone::ErrorCategory category) noexcept {
    switch (category) {
    case moonstone::ErrorCategory::InvalidInput:
        return MS_V1_INVALID_INPUT;
    case moonstone::ErrorCategory::Unsupported:
        return MS_V1_UNSUPPORTED;
    case moonstone::ErrorCategory::BudgetExhausted:
        return MS_V1_BUDGET_EXHAUSTED;
    case moonstone::ErrorCategory::NotFound:
        return MS_V1_NOT_FOUND;
    case moonstone::ErrorCategory::Internal:
        return MS_V1_INTERNAL_ERROR;
    }
    return MS_V1_INTERNAL_ERROR;
}
} // namespace

extern "C" {
uint32_t MOONSTONE_C_CALL ms_v1_api_version(void) { return MOONSTONE_API_VERSION; }

ms_v1_status MOONSTONE_C_CALL ms_v1_source_create(const char *file, size_t file_size,
                                                  const char *text, size_t text_size,
                                                  size_t max_bytes, ms_v1_source **out_source) {
    if (!out_source)
        return MS_V1_INVALID_INPUT;
    *out_source = nullptr;
    if ((!file && file_size) || (!text && text_size))
        return MS_V1_INVALID_INPUT;
    if (file_size > max_bytes || text_size > max_bytes - file_size)
        return MS_V1_BUDGET_EXHAUSTED;
    try {
        auto source = std::make_unique<ms_v1_source>(max_bytes);
        auto id = source->manager.add(std::string(file ? file : "", file_size),
                                      std::string(text ? text : "", text_size));
        if (!id)
            return status(id.error().category);
        source->id = id.value();
        *out_source = source.release();
        return MS_V1_OK;
    } catch (const std::bad_alloc &) {
        return MS_V1_OUT_OF_MEMORY;
    } catch (...) {
        return MS_V1_INTERNAL_ERROR;
    }
}

ms_v1_status MOONSTONE_C_CALL ms_v1_source_span(const ms_v1_source *source, size_t begin,
                                                size_t end, ms_v1_span *out_span) {
    if (!out_span)
        return MS_V1_INVALID_INPUT;
    *out_span = {};
    if (!source)
        return MS_V1_INVALID_INPUT;
    try {
        auto span = source->manager.span(source->id, begin, end);
        if (!span)
            return status(span.error().category);
        const auto &value = span.value();
        *out_span = {value.line, value.column, value.end_line, value.end_column};
        return MS_V1_OK;
    } catch (const std::bad_alloc &) {
        return MS_V1_OUT_OF_MEMORY;
    } catch (...) {
        return MS_V1_INTERNAL_ERROR;
    }
}

void MOONSTONE_C_CALL ms_v1_source_destroy(ms_v1_source *source) { delete source; }
} // extern "C"
