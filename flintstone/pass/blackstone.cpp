#include "../internal.hpp"
#include <blackstone.hpp>
#include <algorithm>
#include <cstdlib>

namespace flintstone {
namespace {
void load_catalog(blackstone::Runtime& runtime, const std::string& directory) {
    runtime.load_directory(directory.empty() ? default_kernel_directory() : directory);
}

// SSA can represent the opcode of a host extension, but not its dialect's
// attributes or effects. Never round-trip such operations through a rewrite.
void require_builtin_ssa(const Function& function, const Registry& registry) {
    for (const auto& block : function.blocks) for (const auto& op : block.operations) {
        const auto code = registry.operation(op.name).ssa_opcode;
        bool canonical = false;
        if (code) {
            const auto name = detail::ssa_operation_name(*code);
            canonical = op.name == name || op.name == "vm." + name.substr(name.find('.') + 1);
        }
        detail::check(canonical, "blackstone.operation", "lower operation before SSA optimization: " + op.name, op.location);
    }
}
}

std::string default_kernel_directory() {
    if (const char* env = std::getenv("FLINTSTONE_KERNEL_DIR"); env && *env) return env;
    if (auto installed = detail::installed_data_directory("blackstone/kernels"); !installed.empty()) return installed;
    return FLINTSTONE_KERNEL_DIR;
}

std::vector<std::string> blackstone_kernel_names(const std::string& directory) {
    blackstone::Runtime runtime;
    load_catalog(runtime, directory);
    std::vector<std::string> names;
    for (const auto& kernel : runtime.kernels())
        if (kernel.ir == "ssa") names.push_back(kernel.name);
    std::sort(names.begin(), names.end());
    return names;
}

Pass blackstone_pass(std::string directory, std::vector<std::string> kernels) {
    return blackstone_pipeline(std::move(directory), std::move(kernels), 1);
}

Pass blackstone_pipeline(std::string directory, std::vector<std::string> kernels, std::size_t max_rounds) {
    detail::check(max_rounds > 0 && max_rounds <= 64, "blackstone.rounds", "expected a round limit in [1, 64]");
    return {"blackstone", {}, {},
        [directory = std::move(directory), kernels = std::move(kernels), max_rounds]
        (Module& module, const Registry& registry, AnalysisManager&) {
            detail::check(module.level != Level::High, "blackstone.level", "lower high-level operations before Blackstone optimization");
            if (kernels.empty()) return false;
            blackstone::Runtime runtime;
            load_catalog(runtime, directory);
            // Check names even in a declaration-only module.
            const auto catalog = runtime.kernels();
            for (const auto& name : kernels) {
                detail::check(std::any_of(catalog.begin(), catalog.end(), [&](const auto& kernel) {
                    return kernel.name == name && kernel.ir == "ssa";
                }), "blackstone.kernel", "missing SSA kernel " + name);
            }
            const auto before = print(module);
            for (auto& function : module.functions) if (!function.external) {
                require_builtin_ssa(function, registry);
                auto graph = to_ssa(module, function, registry);
                auto result = runtime.run_pipeline(kernels, graph, blackstone::IntegerSemantics::Modular, max_rounds);
                if (result.changed) function = detail::from_ssa(graph, function);
            }
            return print(module) != before;
        }};
}

std::vector<PassReport> optimize(Module& module, const Registry& registry) {
    PassManager manager;
    manager.add(blackstone_pipeline("", {
        "fold_add", "fold_sub", "fold_mul", "fold_and", "fold_or", "fold_xor", "fold_neg", "fold_not",
        "add_zero", "sub_zero", "mul_one", "mul_zero", "sub_self", "xor_self",
        "ext_zero_minus_x", "ext_multiply_negative_one", "double_negation",
        "cse_integer_arithmetic", "cse_integer_bitwise", "strength_reduction",
        "copy_propagation", "constant_propagation"
    }, 4));
    manager.add(dead_code_elimination());
    auto candidate = module;
    if (candidate.level == Level::High) lower(candidate, registry, Level::Core);
    auto reports = manager.run(candidate, registry);
    module = std::move(candidate);
    return reports;
}
} // namespace flintstone
