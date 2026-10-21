#include "Flintstone.h"
#include "internal.hpp"
#include <xbind.h>
#include <array>
#include <cstring>
#include <new>

struct flintstone_context { flintstone::Registry registry; };
struct flintstone_module { flintstone::Module module; std::string text; };
namespace {
thread_local std::array<char, 8192> last_error{};
void error_text(const char* message) noexcept {
    std::size_t n = 0;
    if (message) while (message[n] && n + 1 < last_error.size()) { last_error[n] = message[n]; ++n; }
    last_error[n] = '\0';
}
template<class Function> flintstone_status protect(Function&& function) noexcept {
    last_error[0] = '\0';
    try { function(); return FLINTSTONE_OK; }
    catch (const std::bad_alloc&) { error_text("out of memory"); return FLINTSTONE_OUT_OF_MEMORY; }
    catch (const std::exception& e) { error_text(e.what()); return FLINTSTONE_ERROR; }
    catch (...) { error_text("unknown host exception"); return FLINTSTONE_ERROR; }
}
flintstone_status invalid() noexcept { error_text("invalid argument"); return FLINTSTONE_INVALID_ARGUMENT; }
}
namespace flintstone {
std::vector<std::string> binding_targets() {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < xbind_target_count(); ++i) {
        const auto* target = xbind_target_at(i);
        detail::check(target != nullptr, "xbind.target", "binding target " + std::to_string(i) + " returned null");
        result.emplace_back(target->name);
    }
    return result;
}
std::string binding_spec(const std::string& directory) {
    std::array<char, 4096> path{};
    auto status = xbind_resolve_spec(directory.c_str(), "Flintstone", path.data(), path.size());
    detail::check(status == XBIND_STATUS_OK, "xbind.spec", xbind_status_string(status)); return path.data();
}
}
extern "C" {
const char* flintstone_version(void) { last_error[0] = '\0'; return "0.1.0"; }
const char* flintstone_last_error(void) { return last_error.data(); }
flintstone_context* flintstone_context_create(void) {
    flintstone_context* result = nullptr; protect([&] { result = new flintstone_context; }); return result;
}
void flintstone_context_destroy(flintstone_context* context) { delete context; }
void flintstone_module_destroy(flintstone_module* module) { delete module; }
flintstone_status flintstone_parse(flintstone_context* context, const char* source, size_t size, const char* filename, flintstone_module** out) {
    if (out) *out = nullptr;
    if (!context || (!source && size) || !out) return invalid();
    return protect([&] {
        auto result = std::make_unique<flintstone_module>();
        result->module = flintstone::parse(std::string_view(source ? source : "", size), filename ? filename : "<input>");
        *out = result.release();
    });
}
flintstone_module* flintstone_parse_text(flintstone_context* context, const char* source) {
    if (!source) { invalid(); return nullptr; }
    flintstone_module* result = nullptr;
    flintstone_parse(context, source, std::strlen(source), "<input>", &result); return result;
}
flintstone_status flintstone_verify(flintstone_context* context, const flintstone_module* module) {
    if (!context || !module) return invalid();
    return protect([&] { flintstone::require_valid(module->module, context->registry); });
}
flintstone_status flintstone_resolve_imports(flintstone_context* context, flintstone_module* module, const char* stdlib_dir) {
    if (!context || !module) return invalid();
    return protect([&] {
        std::vector<std::string> dirs;
        if (stdlib_dir && *stdlib_dir) dirs.push_back(stdlib_dir);
        flintstone::resolve_imports(module->module, context->registry, dirs);
        module->text.clear();
    });
}
flintstone_status flintstone_lower(flintstone_context* context, flintstone_module* module, flintstone_level level) {
    if (!context || !module || level < FLINTSTONE_HIGH || level > FLINTSTONE_MACHINE) return invalid();
    return protect([&] { flintstone::lower(module->module, context->registry, static_cast<flintstone::Level>(level)); module->text.clear(); });
}
flintstone_status flintstone_optimize(flintstone_context* context, flintstone_module* module, const char* directory, const char* kernel) {
    if (!context || !module || !directory || !kernel) return invalid();
    return protect([&] {
        auto candidate = module->module;
        if (candidate.level == flintstone::Level::High) flintstone::lower(candidate, context->registry, flintstone::Level::Core);
        flintstone::PassManager manager;
        manager.add(flintstone::blackstone_pass(directory, {kernel})); manager.add(flintstone::dead_code_elimination());
        manager.run(candidate, context->registry); module->module = std::move(candidate); module->text.clear();
    });
}
flintstone_status flintstone_print(flintstone_module* module, const char** text) {
    if (text) *text = nullptr;
    if (!module || !text) return invalid();
    return protect([&] { auto result = flintstone::print(module->module); module->text.swap(result); *text = module->text.c_str(); });
}
const char* flintstone_module_text(flintstone_module* module) {
    const char* text = nullptr; flintstone_print(module, &text); return text;
}
flintstone_status flintstone_run_i64(flintstone_context* context, const flintstone_module* module, const char* name,
                                   const int64_t* arguments, size_t count, size_t budget, int64_t* result) {
    if (!context || !module || !name || (!arguments && count) || !result) return invalid();
    return protect([&] {
        auto concrete = flintstone::instantiate(module->module, context->registry);
        const auto& f = concrete.function(name);
        flintstone::detail::check(f.result.kind == flintstone::TypeKind::Integer && f.parameters.size() == count, "c.execution", "integer entry/signature required");
        std::vector<firestone::vm::Value> values;
        for (size_t n = 0; n < count; ++n) {
            flintstone::detail::check(f.parameters[n].type.kind == flintstone::TypeKind::Integer, "c.execution", "integer arguments required");
            if (f.parameters[n].type.bits == 1) {
                flintstone::detail::check(arguments[n] == 0 || arguments[n] == 1, "c.execution", "Boolean argument must be zero or one"); values.emplace_back(arguments[n] != 0);
            } else values.emplace_back(arguments[n]);
        }
        auto answer = flintstone::execute(concrete, context->registry, name, values, budget);
        *result = std::holds_alternative<bool>(answer) ? std::int64_t(std::get<bool>(answer)) : std::get<std::int64_t>(answer);
    });
}
size_t flintstone_binding_target_count(void) { last_error[0] = '\0'; return xbind_target_count(); }
const char* flintstone_binding_target_at(size_t index) {
    last_error[0] = '\0'; const auto* target = xbind_target_at(index);
    if (!target) { invalid(); return nullptr; } return target->name;
}
}
