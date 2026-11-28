#include "mahak/Plugin.hpp"

#include <atomic>
#include <dlfcn.h>
#include <exception>
#include <mutex>
#include <set>
#include <utility>

namespace moonstone::mahak::v1 {
namespace {

Error plugin_error(std::string message, ErrorCategory category = ErrorCategory::InvalidInput) {
    return {category, std::move(message), {}};
}

bool has_required_hook(PluginKind kind, const PluginHooks &hooks) {
    switch (kind) {
    case PluginKind::FixtureGenerator: return hooks.generate != nullptr;
    case PluginKind::Oracle: return hooks.judge != nullptr;
    case PluginKind::Measurement: return hooks.measure != nullptr;
    case PluginKind::Analysis: return hooks.analyze != nullptr;
    case PluginKind::Predictor: return hooks.predict != nullptr;
    case PluginKind::Reporter: return hooks.report != nullptr;
    }
    return false;
}

struct Entry {
    PluginInfo info;
    PluginHooks hooks;
    void *user_data;
};

struct Library {
    void *handle = nullptr;
    PluginRegistration registration;
    std::vector<Entry> entries;

    void finish() noexcept {
        if (auto callback = std::exchange(registration.teardown, nullptr))
            callback(registration.user_data);
    }

    ~Library() {
        finish();
        if (handle)
            dlclose(handle);
    }
};

} // namespace

struct PluginRegistry::Impl {
    PluginHostApi host;
    std::atomic<std::size_t> stopping{0};
    std::mutex mutex;
    std::vector<std::unique_ptr<Library>> libraries;
    std::size_t count = 0;

    const Entry *find(std::string_view id) const {
        for (const auto &library : libraries)
            for (const auto &entry : library->entries)
                if (entry.info.id == id)
                    return &entry;
        return nullptr;
    }
};

PluginRegistry::PluginRegistry() : impl_(std::make_unique<Impl>()) {}
PluginRegistry::~PluginRegistry() { teardown(); }
PluginRegistry::PluginRegistry(PluginRegistry &&) noexcept = default;

PluginRegistry &PluginRegistry::operator=(PluginRegistry &&other) noexcept {
    if (this != &other) {
        teardown();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Result<PluginRegistry> PluginRegistry::create(PluginHostApi host) {
    if (host.struct_size < sizeof(host) || host.abi_version != mahak_plugin_abi_version)
        return plugin_error("incompatible plugin host API");
    if ((host.allocate == nullptr) != (host.deallocate == nullptr))
        return plugin_error("plugin host allocator and deallocator must be paired");
    PluginHostApi defaults;
    mahak_sdk_fill_host_api(&defaults);
    // The registry stores only the v1 prefix by value.
    host.struct_size = sizeof(host);
    if (!host.allocate) {
        host.allocate = defaults.allocate;
        host.deallocate = defaults.deallocate;
    }
    if (!host.log)
        host.log = defaults.log;
    PluginRegistry registry;
    registry.impl_->host = host;
    return registry;
}

Result<void> PluginRegistry::load(std::string_view path, const PluginLoadOptions &options) {
    if (!impl_ || impl_->stopping.load())
        return plugin_error("plugin registry is unavailable");
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping.load())
        return plugin_error("plugin registry is tearing down");
    if (path.empty() || path.find('\0') != std::string_view::npos)
        return plugin_error("invalid plugin library path");
    const std::string filename(path);
    auto fail = [&](std::string reason) { return plugin_error(filename + ": " + reason); };
    if (impl_->count >= options.max_plugins)
        return fail("plugin count limit reached");

    auto library = std::make_unique<Library>();
    library->handle = dlopen(filename.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library->handle) {
        const char *error = dlerror();
        return fail(error ? error : "dlopen failed");
    }
    for (const auto &loaded : impl_->libraries)
        if (loaded->handle == library->handle)
            return fail("duplicate plugin library");
    dlerror();
    void *symbol = dlsym(library->handle, mahak_plugin_entry_symbol);
    const char *symbol_error = dlerror();
    if (symbol_error || !symbol)
        return fail(symbol_error ? symbol_error : "null mahak_plugin_init_v1 symbol");
    if (impl_->stopping.load())
        return fail("plugin registry is tearing down");

    auto init = reinterpret_cast<PluginInit>(symbol); // POSIX dlsym function conversion.
    const int status = init(impl_->host, library->registration);
    auto &registration = library->registration;
    if (registration.struct_size < sizeof(registration)) {
        // An incompatible record cannot supply a trusted teardown pair.
        registration.teardown = nullptr;
        return fail("undersized plugin registration");
    }
    if (status != 0)
        return fail("plugin initialization failed (" + std::to_string(status) + ")");
    if (!registration.count || registration.count > options.max_plugins - impl_->count)
        return fail("invalid plugin registration count or count limit exceeded");
    if (!registration.descriptors || !registration.hooks)
        return fail("missing plugin descriptor or hook table");

    std::set<std::string_view> ids;
    for (std::size_t i = 0; i < registration.count; ++i) {
        const auto &descriptor = registration.descriptors[i];
        const auto &hooks = registration.hooks[i];
        // These are array elements: a different size implies an incompatible stride.
        if (descriptor.struct_size != sizeof(descriptor) || hooks.struct_size != sizeof(hooks))
            return fail("incompatible plugin descriptor or hook size");
        if (descriptor.abi_version != mahak_plugin_abi_version)
            return fail("incompatible plugin ABI version");
        const auto id = descriptor.id.value();
        if (!valid_plugin_id(id))
            return fail("invalid plugin id");
        if (impl_->find(id) || !ids.insert(id).second)
            return fail("duplicate plugin id '" + std::string(id) + "'");
        if (!has_required_hook(descriptor.kind, hooks))
            return fail("invalid plugin kind or missing mandatory hook for '" + std::string(id) + "'");
        library->entries.push_back({{std::string(id), std::string(descriptor.version.value()),
                                     std::string(descriptor.description.value()), descriptor.kind,
                                     descriptor.capabilities, filename}, hooks, descriptor.user_data});
    }
    const auto count = registration.count;
    impl_->libraries.push_back(std::move(library));
    impl_->count += count;
    return {};
}

std::vector<PluginInfo> PluginRegistry::list() const {
    std::vector<PluginInfo> result;
    if (!impl_ || impl_->stopping.load())
        return result;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->stopping.load())
        for (const auto &library : impl_->libraries)
            for (const auto &entry : library->entries)
                result.push_back(entry.info);
    return result;
}

const PluginHooks *PluginRegistry::hooks(std::string_view id) const {
    if (!impl_ || impl_->stopping.load())
        return nullptr;
    std::lock_guard lock(impl_->mutex);
    const auto *entry = impl_->stopping.load() ? nullptr : impl_->find(id);
    return entry ? &entry->hooks : nullptr;
}

const PluginInfo *PluginRegistry::info(std::string_view id) const {
    if (!impl_ || impl_->stopping.load())
        return nullptr;
    std::lock_guard lock(impl_->mutex);
    const auto *entry = impl_->stopping.load() ? nullptr : impl_->find(id);
    return entry ? &entry->info : nullptr;
}

Result<OracleResult> PluginRegistry::judge(std::string_view id, const CasePlan &plan,
                                           const CommandResult &command) const {
    if (!impl_ || impl_->stopping.load())
        return plugin_error("plugin registry is unavailable", ErrorCategory::Internal);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping.load())
        return plugin_error("plugin registry is tearing down", ErrorCategory::Internal);
    const auto *entry = impl_->find(id);
    const std::string oracle = "plugin oracle '" + std::string(id) + "'";
    if (!entry || entry->info.kind != PluginKind::Oracle)
        return plugin_error(oracle + " is not registered", ErrorCategory::NotFound);
    try {
        auto result = entry->hooks.judge(entry->user_data, plan, command);
        if (result.validity < Validity::Valid || result.validity > Validity::MeasurementFailed ||
            result.passed != (result.validity == Validity::Valid) ||
            (!result.passed && result.validity == Validity::InvalidOutput && result.message.empty()))
            return plugin_error(oracle + " returned an inconsistent or default verdict",
                                ErrorCategory::Internal);
        return result;
    } catch (const std::exception &error) {
        return plugin_error(oracle + " threw: " + error.what(), ErrorCategory::Internal);
    } catch (...) {
        return plugin_error(oracle + " threw", ErrorCategory::Internal);
    }
}

void PluginRegistry::teardown() {
    if (!impl_)
        return;
    ++impl_->stopping;
    std::lock_guard lock(impl_->mutex);
    for (auto &library : impl_->libraries)
        library->finish();
    impl_->libraries.clear();
    impl_->count = 0;
    --impl_->stopping;
}

} // namespace moonstone::mahak::v1
