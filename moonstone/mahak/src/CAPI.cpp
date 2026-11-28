#include "mahak/mahak.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <string>

#include "mahak/Engine.hpp"
#include "mahak/Manifest.hpp"
#include "mahak/Storage.hpp"
#include "mahak/Version.hpp"

namespace {

char *duplicate(const std::string &text) {
    const std::size_t size = text.size() + 1;
    char *buffer = static_cast<char *>(std::malloc(size));
    if (buffer == nullptr)
        return nullptr;
    std::memcpy(buffer, text.c_str(), size);
    return buffer;
}

int classify(moonstone::ErrorCategory category) {
    using moonstone::ErrorCategory;
    switch (category) {
    case ErrorCategory::InvalidInput:
        return 2;
    case ErrorCategory::NotFound:
    case ErrorCategory::Unsupported:
        return 5;
    case ErrorCategory::BudgetExhausted:
    case ErrorCategory::Internal:
        return 1;
    }
    return 1;
}

} // namespace

struct mahak_engine {
    moonstone::mahak::Engine engine;
    std::mutex mutex;
};

extern "C" {

const char *mahak_version(void) {
    return moonstone::mahak::mahak_version_string;
}

unsigned mahak_api_version(void) {
    return moonstone::mahak::mahak_api_version;
}

const char *mahak_result_schema(void) {
    static const std::string schema = moonstone::mahak::result_schema_json();
    return schema.c_str();
}

mahak_engine *mahak_engine_create(void) {
    try {
        return new mahak_engine();
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
}

void mahak_engine_destroy(mahak_engine *engine) {
    delete engine;
}

int mahak_validate_manifest(const char *path, char **out_error) {
    if (out_error != nullptr)
        *out_error = nullptr;
    if (path == nullptr) {
        if (out_error != nullptr)
            *out_error = duplicate("mahak_validate_manifest: path is null");
        return 2;
    }
    try {
        auto manifest = moonstone::mahak::load_manifest(path);
        if (!manifest) {
            if (out_error != nullptr)
                *out_error = duplicate(manifest.error().message);
            // mahak_validate_manifest reports every failure as a configuration
            // error (2), including a missing or unreadable manifest.
            return 2;
        }
        const auto diagnostics = moonstone::mahak::validate_manifest(manifest.value());
        const std::string formatted = moonstone::mahak::format_diagnostics(diagnostics);
        for (const auto &diagnostic : diagnostics) {
            if (diagnostic.severity == moonstone::Severity::Error) {
                if (out_error != nullptr)
                    *out_error = duplicate(formatted);
                return 2;
            }
        }
        return 0;
    } catch (const std::exception &error) {
        if (out_error != nullptr)
            *out_error = duplicate(std::string("internal error: ") + error.what());
        return 1;
    } catch (...) {
        if (out_error != nullptr)
            *out_error = duplicate("internal error: unknown exception");
        return 1;
    }
}

int mahak_run_manifest(mahak_engine *engine, const char *path, char **out_json,
                       char **out_error) {
    if (out_json != nullptr)
        *out_json = nullptr;
    if (out_error != nullptr)
        *out_error = nullptr;
    if (engine == nullptr) {
        if (out_error != nullptr)
            *out_error = duplicate("mahak_run_manifest: engine is null");
        return 1;
    }
    if (path == nullptr) {
        if (out_error != nullptr)
            *out_error = duplicate("mahak_run_manifest: path is null");
        return 2;
    }
    try {
        std::lock_guard<std::mutex> guard(engine->mutex);
        auto session = engine->engine.run_file(path);
        if (!session) {
            if (out_error != nullptr)
                *out_error = duplicate(session.error().message);
            return classify(session.error().category);
        }
        auto text = moonstone::mahak::encode_session_text(session.value(), true);
        if (!text) {
            if (out_error != nullptr)
                *out_error = duplicate(text.error().message);
            return 1;
        }
        if (out_json != nullptr)
            *out_json = duplicate(text.value());
        const std::string &status = session.value().status;
        return status == "completed" ? 0 : 3;
    } catch (const std::exception &error) {
        if (out_error != nullptr)
            *out_error = duplicate(std::string("internal error: ") + error.what());
        return 1;
    } catch (...) {
        if (out_error != nullptr)
            *out_error = duplicate("internal error: unknown exception");
        return 1;
    }
}

void mahak_engine_cancel(mahak_engine *engine) {
    if (engine == nullptr)
        return;
    engine->engine.cancel();
}

void mahak_free(char *value) {
    std::free(value);
}

} // extern "C"
