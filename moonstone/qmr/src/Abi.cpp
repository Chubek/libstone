#include "qmr/qmr_abi.h"
#include "qmr/Qmr.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace {

using moonstone::qmr::Codec;
using moonstone::qmr::Context;
using moonstone::qmr::Engine;
using moonstone::qmr::Limits;
using moonstone::qmr::Options;

constexpr char kVersion[] = "Moonstone QMR 0.2 (ABI 1)";

char *duplicate(const std::string &text) {
    char *buffer = static_cast<char *>(std::malloc(text.size() + 1));
    if (!buffer) return nullptr;
    std::memcpy(buffer, text.data(), text.size());
    buffer[text.size()] = '\0';
    return buffer;
}

Context build_context(const char *const *keys, const char *const *values, size_t count) {
    Context result;
    for (size_t i = 0; i < count; ++i) {
        std::string key(keys[i]);
        if (!values || !values[i]) result[key] = nullptr;
        else result[key] = std::string(values[i]);
    }
    return result;
}

Codec to_codec(int codec) {
    switch (codec) {
    case QMR_CODEC_HTML: return Codec::Html;
    case QMR_CODEC_PLAIN: return Codec::Plain;
    case QMR_CODEC_JSON: return Codec::Json;
    case QMR_CODEC_XML: return Codec::Xml;
    default: throw std::invalid_argument("unknown codec");
    }
}

Limits to_limits(const qmr_limits *limits) {
    Limits result;
    if (!limits) return result;
    auto assign = [](size_t &field, size_t incoming, size_t fallback) {
        field = incoming == 0 ? fallback : incoming;
    };
    assign(result.max_template_bytes, limits->max_template_bytes,
           Limits{}.max_template_bytes);
    assign(result.max_compiled_bytes, limits->max_compiled_bytes,
           Limits{}.max_compiled_bytes);
    assign(result.max_output_bytes, limits->max_output_bytes,
           Limits{}.max_output_bytes);
    assign(result.max_context_bytes, limits->max_context_bytes,
           Limits{}.max_context_bytes);
    assign(result.max_nodes, limits->max_nodes, Limits{}.max_nodes);
    assign(result.max_include_depth, limits->max_include_depth,
           Limits{}.max_include_depth);
    assign(result.max_instructions, limits->max_instructions,
           Limits{}.max_instructions);
    assign(result.max_value_depth, limits->max_value_depth,
           Limits{}.max_value_depth);
    assign(result.max_memory_bytes, limits->max_memory_bytes,
           Limits{}.max_memory_bytes);
    result.max_duration = std::chrono::milliseconds(limits->max_duration_ms);
    return result;
}

} // namespace

// Definition of the opaque handle declared by qmr_abi.h; at global scope so
// it is the same type the header's typedef names.
struct qmr_engine {
    moonstone::qmr::Options options;
};

extern "C" {

int qmr_abi_version(void) {
    return QMR_ABI_VERSION;
}

const char *qmr_version_string(void) {
    return kVersion;
}

qmr_engine *qmr_engine_create(const qmr_limits *limits, int codec) {
    try {
        Options options;
        options.limits = to_limits(limits);
        options.codec = to_codec(codec);
        return new qmr_engine{std::move(options)};
    } catch (const std::exception &) {
        return nullptr;
    }
}

void qmr_engine_destroy(qmr_engine *engine) {
    delete engine;
}

int qmr_engine_render(qmr_engine *engine, const char *template_text,
                      const char *template_name, const char *const *keys,
                      const char *const *values, size_t count,
                      char **out, char **error) {
    if (!engine || !template_text || (!out && !error)) return -1;
    try {
        const std::string rendered = Engine(engine->options).render(
            template_text, build_context(keys, values, count),
            template_name ? template_name : "<template>");
        if (out) {
            char *buffer = duplicate(rendered);
            if (!buffer) {
                if (error) *error = duplicate("out of memory");
                return -2;
            }
            *out = buffer;
        }
        return 0;
    } catch (const std::exception &e) {
        if (error) {
            char *buffer = duplicate(e.what());
            if (!buffer) return -2;
            *error = buffer;
        }
        return 1;
    }
}

int qmr_engine_render_file(qmr_engine *engine, const char *path, const char *root,
                           const char *const *keys, const char *const *values,
                           size_t count, char **out, char **error) {
    if (!engine || !path || (!out && !error)) return -1;
    try {
        const std::string rendered = Engine(engine->options).render_file(
            path, build_context(keys, values, count),
            root ? std::filesystem::path(root) : std::filesystem::path());
        if (out) {
            char *buffer = duplicate(rendered);
            if (!buffer) {
                if (error) *error = duplicate("out of memory");
                return -2;
            }
            *out = buffer;
        }
        return 0;
    } catch (const std::exception &e) {
        if (error) {
            char *buffer = duplicate(e.what());
            if (!buffer) return -2;
            *error = buffer;
        }
        return 1;
    }
}

void qmr_free(char *buffer) {
    std::free(buffer);
}

} // extern "C"
