#include "driver_io.hpp"
#include "embedding_internal.hpp"

#include <cephyr/Cephyr.h>
#include <cephyr/diagnostics.hpp>

#include <dlfcn.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct cephyr_context {
    std::vector<std::string> arguments;
    std::vector<const char *> argv;
    std::optional<std::string> resource_root, archon_root;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<const cephyr::detail::TranslationUnit>> units;
    cephyr::detail::HostSymbols symbols;
};

struct cephyr_module {
    cephyr::detail::MemoryFile memory{"cephyr-module.so"};
    // Reserve the fallback pathname for at least the lifetime of the handle.
    std::optional<cephyr::detail::TempDirectory> directory;
    void *handle = nullptr;

    ~cephyr_module()
    {
        if (handle)
            dlclose(handle);
    }
};

namespace {
cephyr_request context_request(const cephyr_context &context, const char *source,
                               size_t size, const cephyr_sdk *sdk)
{
    return {sizeof(cephyr_request), context.argv.size(), context.argv.data(), source, size,
            context.resource_root ? context.resource_root->c_str() : nullptr,
            context.archon_root ? context.archon_root->c_str() : nullptr, sdk};
}
}

int cephyr_context_create(const cephyr_context_options *options, cephyr_context **context)
{
    if (!context)
        return CEPHYR_INVALID_ARGUMENT;
    *context = nullptr;
    if (!options || options->struct_size < sizeof(cephyr_context_options) ||
        (options->argc && !options->argv) ||
        (options->resource_root && !*options->resource_root) ||
        (options->archon_root && !*options->archon_root))
        return CEPHYR_INVALID_ARGUMENT;
    for (std::size_t i = 0; i < options->argc; ++i)
        if (!options->argv[i])
            return CEPHYR_INVALID_ARGUMENT;

    try {
        auto storage = std::make_unique<cephyr_context>();
        storage->arguments.reserve(options->argc);
        storage->argv.reserve(options->argc);
        for (std::size_t i = 0; i < options->argc; ++i)
            storage->arguments.emplace_back(options->argv[i]);
        for (const auto &arg : storage->arguments)
            storage->argv.push_back(arg.c_str());
        if (options->resource_root)
            storage->resource_root.emplace(options->resource_root);
        if (options->archon_root)
            storage->archon_root.emplace(options->archon_root);
        *context = storage.release();
        return CEPHYR_OK;
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

int cephyr_context_compile(const cephyr_context *context, const char *source, size_t source_size,
                           const cephyr_sdk *sdk, cephyr_result **result)
{
    if (!context) {
        if (result)
            *result = nullptr;
        return CEPHYR_INVALID_ARGUMENT;
    }
    const auto request = context_request(*context, source, source_size, sdk);
    return cephyr_compile(&request, result);
}

void cephyr_context_destroy(cephyr_context *context) { delete context; }

int cephyr_context_add_translation_unit(cephyr_context *context, const char *name,
                                        const char *source, size_t source_size)
{
    if (!context || (name && !*name) || (!source && source_size) ||
        source_size > cephyr::detail::max_source_size)
        return CEPHYR_INVALID_ARGUMENT;
    try {
        auto unit = std::make_shared<cephyr::detail::TranslationUnit>();
        unit->source.assign(source ? source : "", source_size);
        if (name)
            unit->name = name;
        const std::lock_guard guard(context->mutex);
        if (!name)
            unit->name = "<cephyr-tu-" + std::to_string(context->units.size() + 1) + ">";
        context->units.push_back(std::move(unit));
        return CEPHYR_OK;
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

int cephyr_context_link(const cephyr_context *context, const cephyr_sdk *sdk,
                        cephyr_result **result)
{
    if (!result)
        return CEPHYR_INVALID_ARGUMENT;
    *result = nullptr;
    if (!context)
        return CEPHYR_INVALID_ARGUMENT;
    try {
        decltype(context->units) units;
        cephyr::detail::HostSymbols symbols;
        {
            const std::lock_guard guard(context->mutex);
            if (context->units.empty())
                return CEPHYR_INVALID_ARGUMENT;
            units = context->units;
            symbols = context->symbols;
        }
        const auto request = context_request(*context, nullptr, 0, sdk);
        return cephyr::detail::compile_units(&request, units, &symbols, result);
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

int cephyr_context_reset(cephyr_context *context)
{
    if (!context)
        return CEPHYR_INVALID_ARGUMENT;
    try {
        const std::lock_guard guard(context->mutex);
        context->units.clear();
        context->symbols.clear();
        return CEPHYR_OK;
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

int cephyr_context_register_host_symbol(cephyr_context *context, const char *name,
                                        cephyr_host_symbol_kind kind, const void *address)
{
    if (!context || !address || !name ||
        (kind != CEPHYR_HOST_FUNCTION && kind != CEPHYR_HOST_DATA))
        return CEPHYR_INVALID_ARGUMENT;
    const auto letter = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
    };
    const std::string_view symbol(name);
    if (symbol.empty() || !letter(symbol.front()) || symbol.starts_with("__wrap_") ||
        symbol.starts_with("__real_"))
        return CEPHYR_INVALID_ARGUMENT;
    for (const char ch : symbol)
        if (!letter(ch) && !(ch >= '0' && ch <= '9'))
            return CEPHYR_INVALID_ARGUMENT;
    try {
        const std::lock_guard guard(context->mutex);
        context->symbols.insert_or_assign(std::string(symbol), cephyr::detail::HostSymbol{address, kind});
        return CEPHYR_OK;
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

namespace {

int load_error(cephyr_result **details, const char *message)
{
    if (details) {
        auto result = std::make_unique<cephyr_result>();
        cephyr::Diagnostics diagnostics;
        diagnostics.error("cephyr.module.load", message);
        std::ostringstream text;
        diagnostics.render(text);
        result->diagnostics = std::move(text).str();
        *details = result.release();
    }
    return CEPHYR_LOAD_ERROR;
}

int load_module(const cephyr_result &artifact, cephyr_module **module, cephyr_result **details)
{
    try {
        if (artifact.host_binding_process && artifact.host_binding_process != getpid())
            return load_error(details, "host bindings belong to another process");
        auto storage = std::make_unique<cephyr_module>();
        fs::path file;
        if (storage->memory.fd() >= 0) {
            storage->memory.write(artifact.output);
            // dlopen can match a previous pathname before even opening it.
            // Use a unique spelling despite fd reuse (including after dlclose
            // if the loader retained a NODELETE or otherwise pinned module).
            static std::atomic<std::uint64_t> serial{0};
            auto id = serial.fetch_add(1, std::memory_order_relaxed);
            std::string alias = "/proc/self";
            for (unsigned bit = 0; bit < 64; ++bit, id >>= 1)
                alias += id & 1 ? "/." : "//";
            file = alias + "/fd/" + std::to_string(storage->memory.fd());
        } else {
            storage->directory.emplace(fs::absolute(fs::temp_directory_path()), "cephyr-module-XXXXXX");
            file = storage->directory->path / "module.so";
            cephyr::detail::save(file, artifact.output);
        }
        storage->handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!storage->handle) {
            const char *error = dlerror();
            return load_error(details, error ? error : "dynamic loader rejected the artifact");
        }
        if (storage->directory)
            fs::remove(file);
        *module = storage.release();
        return CEPHYR_OK;
    } catch (const std::system_error &error) {
        return load_error(details, error.what());
    }
}

} // namespace

int cephyr_module_load(const cephyr_result *artifact, cephyr_module **module, cephyr_result **details)
{
    if (details)
        *details = nullptr;
    if (!module)
        return CEPHYR_INVALID_ARGUMENT;
    *module = nullptr;
    if (!artifact || !artifact->loadable_output)
        return CEPHYR_INVALID_ARGUMENT;
    try {
        return load_module(*artifact, module, details);
    } catch (...) {
        return CEPHYR_INTERNAL_ERROR;
    }
}

int cephyr_module_symbol(const cephyr_module *module, const char *name, void **address)
{
    if (!address)
        return CEPHYR_INVALID_ARGUMENT;
    *address = nullptr;
    if (!module || !name || !*name)
        return CEPHYR_INVALID_ARGUMENT;
    dlerror();
    auto *symbol = dlsym(module->handle, name);
    if (dlerror())
        return CEPHYR_SYMBOL_NOT_FOUND;
    *address = symbol;
    return CEPHYR_OK;
}

void cephyr_module_destroy(cephyr_module *module) { delete module; }
