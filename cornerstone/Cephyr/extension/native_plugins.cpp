#include "native_plugins.hpp"

#include <cephyr/diagnostics.hpp>

#include <dlfcn.h>
#include <system_error>

namespace cephyr {
namespace {

bool append_sdk(const cephyr_sdk *sdk, std::vector<cephyr_sdk_hook> &hooks,
                std::vector<cephyr_sdk_directive> &directives,
                Diagnostics &diag, const Location &where)
{
    if (!sdk || sdk->struct_size < offsetof(cephyr_sdk, directives) ||
        sdk->abi_version != CEPHYR_SDK_ABI_VERSION) {
        diag.error("cephyr.plugin.abi", "missing or incompatible SDK descriptor", where);
        return false;
    }
    // Accept the original SDK v1 prefix without reading an absent tail.
    constexpr auto directive_fields_size = offsetof(cephyr_sdk, directive_count) +
                                           sizeof(cephyr_sdk::directive_count);
    const auto count = sdk->struct_size >= directive_fields_size ? sdk->directive_count : 0;
    if (sdk->hook_count > CEPHYR_SDK_MAX_HOOKS - hooks.size() ||
        count > CEPHYR_SDK_MAX_DIRECTIVES - directives.size() ||
        (sdk->hook_count && !sdk->hooks) || (count && !sdk->directives)) {
        diag.error("cephyr.plugin.abi", "SDK registration count exceeds the limit or its array is missing",
                   where);
        return false;
    }
    if (sdk->hook_count)
        hooks.insert(hooks.end(), sdk->hooks, sdk->hooks + sdk->hook_count);
    if (count)
        directives.insert(directives.end(), sdk->directives, sdk->directives + count);
    return true;
}

} // namespace

void NativePlugins::CloseModule::operator()(void *handle) const
{
    dlclose(handle);
}

bool NativePlugins::load(const std::vector<std::filesystem::path> &paths,
                         const cephyr_sdk *request_sdk, Diagnostics &diag)
{
    if (paths.size() > CEPHYR_PLUGIN_MAX_FILES) {
        diag.error("cephyr.plugin.load", "too many native plugins");
        return false;
    }
    if (request_sdk && !append_sdk(request_sdk, hooks_, directives_, diag, {}))
        return false;

    for (const auto &path : paths) {
        const auto &filename = path.native();
        const Location where{filename};
        // dlopen expands dynamic string tokens in its argument. Reject '$'
        // rather than letting the loader reinterpret a configured filename.
        if (!path.is_absolute() || filename.find('$') != filename.npos) {
            diag.error("cephyr.plugin.load", "native plugin requires an explicit absolute file path without '$'",
                       where);
            return false;
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            diag.error("cephyr.plugin.load", error ? "cannot read native plugin: " + error.message()
                                                  : "native plugin must be a regular file", where);
            return false;
        }
        std::unique_ptr<void, CloseModule> handle(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL));
        if (!handle) {
            const char *message = dlerror();
            diag.error("cephyr.plugin.load", message ? message : "cannot open native plugin", where);
            return false;
        }
        dlerror();
        void *symbol = dlsym(handle.get(), CEPHYR_PLUGIN_ENTRY_V1);
        const char *symbol_error = dlerror();
        Dl_info origin{};
        // dlsym can find symbols in dependencies; the configured object
        // itself must export the entry point.
        if (symbol_error || !symbol || !dladdr(symbol, &origin) || !origin.dli_fname ||
            !std::filesystem::equivalent(path, origin.dli_fname, error)) {
            diag.error("cephyr.plugin.abi", "native plugin must export " CEPHYR_PLUGIN_ENTRY_V1, where);
            return false;
        }
        const auto entry = reinterpret_cast<cephyr_plugin_entry_v1>(symbol);
        if (!append_sdk(entry(), hooks_, directives_, diag, where))
            return false;
        handles_.push_back(std::move(handle));
    }
    sdk_.hooks = hooks_.data();
    sdk_.hook_count = hooks_.size();
    sdk_.directives = directives_.data();
    sdk_.directive_count = directives_.size();
    return true;
}

} // namespace cephyr
