#include "../internal.hpp"
#include <libir/LoopAnalysis.hpp>
#include <libvm/BytecodeBinary.hpp>
#include <blackstone.hpp>

namespace flintstone {
void lower(Module& module, const Registry& registry, Level destination, std::size_t limit) {
    detail::check(static_cast<unsigned>(destination) <= static_cast<unsigned>(Level::Machine) &&
                  static_cast<unsigned>(destination) >= static_cast<unsigned>(module.level), "lowering.direction", "invalid lowering direction");
    auto candidate = instantiate(module, registry);
    std::size_t rewrites = 0;
    for (auto& function : candidate.functions) for (auto& block : function.blocks) {
        for (std::size_t index = 0; index < block.operations.size();) {
            const auto& operation = block.operations[index];
            if (static_cast<unsigned>(registry.operation(operation.name).level) >= static_cast<unsigned>(destination)) { ++index; continue; }
            const LoweringRule* selected = nullptr;
            for (const auto& rule : registry.lowerings()) if (rule.operation == operation.name &&
                static_cast<unsigned>(rule.destination) <= static_cast<unsigned>(destination)) { selected = &rule; break; }
            detail::check(selected != nullptr, "lowering.missing", "no conversion rule for " + operation.name, operation.location);
            detail::check(rewrites++ < limit, "lowering.limit", "rewrite budget exhausted", operation.location);
            auto replacement = selected->rewrite(operation, function, candidate, registry);
            detail::check(replacement.size() <= 1000000 && block.operations.size() <= 1000000 - replacement.size(), "lowering.limit", "replacement size limit exceeded", operation.location);
            for (const auto& value : operation.results) {
                std::size_t definitions = 0;
                for (const auto& op : replacement) for (const auto& result : op.results)
                    if (result.id == value.id && result.type == value.type) ++definitions;
                detail::check(definitions == 1, "lowering.result", "replacement must preserve each original result identity/type exactly once", operation.location);
            }
            auto position = block.operations.begin() + static_cast<std::ptrdiff_t>(index);
            position = block.operations.erase(position);
            block.operations.insert(position, replacement.begin(), replacement.end());
        }
    }
    candidate.level = destination;
    require_valid(candidate, registry);
    const bool changed = print(candidate) != print(module);
    candidate.revision = module.revision + (changed ? 1 : 0);
    module = std::move(candidate);
}
Artifact emit(const Module& module, const Registry& registry, const std::string& backend) {
    require_valid(module, registry); return registry.backend(backend)(module, registry);
}
AnalysisManager::AnalysisManager() {
    add("def_use", [](const Module& module, const Registry&) -> std::any {
        DefUse result;
        for (const auto& f : module.functions) for (const auto& b : f.blocks) for (std::size_t n = 0; n < b.operations.size(); ++n)
            for (std::size_t p = 0; p < b.operations[n].operands.size(); ++p) {
                const auto& operand = b.operations[n].operands[p];
                if (operand.kind == OperandKind::Value) result.uses[{f.name, operand.value}].push_back({f.name, b.id, n, p});
            }
        return result;
    });
    add("cfg", [](const Module& module, const Registry& registry) -> std::any {
        std::map<std::string, firestone::ir::ControlFlowGraph> result;
        for (const auto& f : module.functions) if (!f.external) result.emplace(f.name, detail::cfg(f, registry).graph);
        return result;
    });
    add("dominators", [](const Module& module, const Registry& registry) -> std::any {
        std::map<std::string, firestone::ir::DominatorSets> result;
        for (const auto& f : module.functions) if (!f.external) result.emplace(f.name, detail::cfg(f, registry).graph.dominators());
        return result;
    });
    add("loops", [](const Module& module, const Registry& registry) -> std::any {
        std::map<std::string, std::vector<firestone::ir::NaturalLoop>> result;
        for (const auto& f : module.functions) if (!f.external) result.emplace(f.name, firestone::ir::natural_loops(detail::cfg(f, registry).graph));
        return result;
    });
}
void AnalysisManager::add(std::string name, Factory factory) {
    detail::check(!name.empty() && factory && !factories_.count(name), "analysis.registration", "invalid or duplicate analysis");
    factories_.emplace(std::move(name), std::move(factory));
}
const std::any& AnalysisManager::get(const std::string& name, const Module& module, const Registry& registry) {
    auto found = factories_.find(name);
    detail::check(found != factories_.end(), "analysis.unavailable", "unknown analysis " + name);
    if (!cache_.count(name)) cache_[name] = found->second(module, registry);
    return cache_.at(name);
}
void AnalysisManager::invalidate(const std::set<std::string>& preserves) {
    for (auto it = cache_.begin(); it != cache_.end();) if (!preserves.count(it->first)) it = cache_.erase(it); else ++it;
}
void PassManager::add(Pass pass) {
    detail::check(!pass.name.empty() && pass.run, "pass.registration", "invalid pass");
    for (const auto& existing : passes_)
        detail::check(existing.name != pass.name, "pass.registration", "duplicate pass name '" + pass.name + "'");
    passes_.push_back(std::move(pass));
}
void PassManager::add_analysis(std::string name, AnalysisManager::Factory factory) {
    AnalysisManager validation;
    for (const auto& [key, value] : analyses_) validation.add(key, value);
    validation.add(name, factory);
    analyses_.emplace(std::move(name), std::move(factory));
}
std::vector<PassReport> PassManager::run(Module& module, const Registry& registry) const {
    require_valid(module, registry);
    Module candidate = instantiate(module, registry);
    if (print(candidate) != print(module)) candidate.revision = module.revision + 1;
    AnalysisManager analyses;
    for (const auto& [name, factory] : analyses_) analyses.add(name, factory);
    std::vector<PassReport> reports;
    for (const auto& pass : passes_) {
        try {
            for (const auto& name : pass.requirements) analyses.get(name, candidate, registry);
            const auto before = print(candidate);
            const auto revision = candidate.revision;
            const bool reported = pass.run(candidate, registry, analyses);
            require_valid(candidate, registry);
            bool changed = print(candidate) != before;
            detail::check(changed == reported, "pass.contract", "pass changed flag does not match its output");
            candidate.revision = revision + (changed ? 1 : 0);
            if (changed) analyses.invalidate(pass.preserves);
            reports.push_back({pass.name, changed, pass.preserves});
        } catch (const Error& e) {
            Diagnostics diagnostics{{"pass." + pass.name, "pass '" + pass.name + "' failed: " + e.what(), {}, pass.name}};
            for (const auto& d : e.diagnostics()) diagnostics.push_back(d);
            throw Error(std::move(diagnostics));
        } catch (const std::exception& e) { detail::fail("pass." + pass.name, e.what()); }
    }
    module = std::move(candidate); return reports;
}
Pass dead_code_elimination() {
    return {"dead-code-elimination", {}, {"cfg", "dominators", "loops"},
        [](Module& module, const Registry& registry, AnalysisManager&) {
            bool changed = false, progress;
            do {
                progress = false;
                for (auto& f : module.functions) {
                    std::set<ValueId> used;
                    for (const auto& b : f.blocks) for (const auto& op : b.operations) for (const auto& operand : op.operands)
                        if (operand.kind == OperandKind::Value) used.insert(operand.value);
                    for (auto& b : f.blocks) {
                        auto removed = std::erase_if(b.operations, [&](const Operation& op) {
                            const auto& def = registry.operation(op.name);
                            return def.effect == Effect::Pure && !def.terminator && !op.results.empty() &&
                                   std::none_of(op.results.begin(), op.results.end(), [&](const Value& v) { return used.count(v.id); });
                        });
                        progress |= removed != 0;
                    }
                }
                changed |= progress;
            } while (progress);
            return changed;
        }};
}
Pass blackstone_pass(std::string directory, std::vector<std::string> kernels) {
    return {"blackstone", {"cfg", "dominators", "def_use", "loops"}, {},
        [directory = std::move(directory), kernels = std::move(kernels)](Module& module, const Registry& registry, AnalysisManager&) {
            detail::check(module.level != Level::High, "blackstone.level", "lower high-level operations before Blackstone optimization");
            blackstone::Runtime runtime; runtime.load_directory(directory);
            bool changed = false;
            // Validate requested names even for a module with no definitions.
            const auto catalog = runtime.kernels();
            for (const auto& name : kernels) detail::check(std::any_of(catalog.begin(), catalog.end(), [&](const auto& k) { return k.name == name && k.ir == "ssa"; }),
                                                         "blackstone.kernel", "missing SSA kernel " + name);
            for (auto& function : module.functions) if (!function.external) {
                auto graph = to_ssa(module, function, registry);
                bool function_changed = false;
                for (const auto& kernel : kernels) function_changed |= runtime.run(kernel, graph, blackstone::IntegerSemantics::Modular).changed;
                if (function_changed) {
                    auto replacement = detail::from_ssa(graph, function);
                    // All changes remain private to the outer pass transaction.
                    function = std::move(replacement); changed = true;
                }
            }
            return changed;
        }};
}
} // namespace flintstone
