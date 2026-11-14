#include <exoml/StaticFFI.hpp>
#include <memory>

namespace moonstone::exoml {
namespace {
struct Workspace {
    std::map<std::string, BindingModule> modules;
    const BindingModule &get(const std::string &name) const {
        auto found = modules.find(name);
        if (found == modules.end())
            throw std::invalid_argument("unknown binding module: " + name);
        return found->second;
    }
    template <class F> std::string update(const std::string &name, F operation) {
        auto next = get(name);
        operation(next);
        auto diagnostics = binding_diagnostics(next);
        if (!diagnostics.empty())
            throw std::invalid_argument(diagnostics.front().message);
        modules.at(name) = std::move(next);
        return name;
    }
};
CFunction &function(BindingModule &module, const std::string &name) {
    for (auto &item : module.functions)
        if (item.name == name)
            return item;
    throw std::invalid_argument("unknown function: " + name);
}
RecordLayout &layout(BindingModule &module, const std::string &name) {
    for (auto &item : module.layouts)
        if (item.c_type == name)
            return item;
    throw std::invalid_argument("unknown record layout: " + name);
}
const std::map<std::string, Effect> effects = {{"read", Effect::Read},
                                               {"write", Effect::Write},
                                               {"allocate", Effect::Allocate},
                                               {"free", Effect::Free},
                                               {"borrow_shared", Effect::BorrowShared},
                                               {"borrow_exclusive", Effect::BorrowExclusive},
                                               {"escape", Effect::Escape},
                                               {"foreign_call", Effect::ForeignCall},
                                               {"thread_spawn", Effect::ThreadSpawn},
                                               {"suspend", Effect::Suspend}};
} // namespace

void install_static_ffi(Interpreter &interpreter) {
    auto state = std::make_shared<Workspace>();
    NativeOptions pure;
    pure.privileged = false;
    StaticLibrary library("StaticFFI");
    library.bind(
        "Module",
        [state](std::string name) {
            BindingModule module;
            module.name = name;
            auto errors = validate_binding(module);
            if (!errors.empty())
                throw std::invalid_argument(errors.front());
            if (state->modules.size() >= 128)
                throw std::length_error("module budget exceeded");
            if (!state->modules.emplace(name, std::move(module)).second)
                throw std::invalid_argument("duplicate module: " + name);
            return name;
        },
        pure);
    library.bind(
        "Header",
        [state](std::string name, std::string header) {
            return state->update(name, [&](auto &m) { m.headers.push_back(std::move(header)); });
        },
        pure);
    library.bind(
        "Function",
        [state](std::string module, std::string name, std::string returns,
                std::vector<std::string> parameters) {
            return state->update(module, [&](auto &m) {
                CFunction f;
                f.name = std::move(name);
                f.return_type = std::move(returns);
                f.ownership.effects.unknown = true;
                for (std::size_t i = 0; i < parameters.size(); ++i)
                    f.parameters.push_back({"arg" + std::to_string(i), parameters[i]});
                m.functions.push_back(std::move(f));
            });
        },
        pure);
    library.bind(
        "Rename",
        [state](std::string module, std::string name, std::string exported_name) {
            return state->update(module, [&](auto &m) {
                function(m, name).exported_name = std::move(exported_name);
            });
        },
        pure);
    library.bind(
        "Policy",
        [state](std::string module, std::string name, bool privileged, bool thread_affine,
                bool c_strings) {
            return state->update(module, [&](auto &m) {
                auto &f = function(m, name);
                f.privileged = privileged;
                f.thread_affine = thread_affine;
                f.c_strings = c_strings;
            });
        },
        pure);
    library.bind(
        "Document",
        [state](std::string module, std::string name, std::string documentation) {
            return state->update(module, [&](auto &m) {
                function(m, name).documentation = std::move(documentation);
            });
        },
        pure);
    library.bind(
        "Borrow",
        [state](std::string module, std::string name, std::size_t index) {
            return state->update(module, [&](auto &m) {
                function(m, name).ownership.borrowed_arguments.insert(index);
            });
        },
        pure);
    library.bind(
        "Effects",
        [state](std::string module, std::string name, std::vector<std::string> names,
                bool unknown) {
            return state->update(module, [&](auto &m) {
                auto &summary = function(m, name).ownership.effects;
                summary.effects.clear();
                summary.unknown = unknown;
                for (const auto &effect : names) {
                    auto found = effects.find(effect);
                    if (found == effects.end())
                        throw std::invalid_argument("unknown effect: " + effect);
                    summary.effects.insert(found->second);
                }
            });
        },
        pure);
    library.bind(
        "Constant",
        [state](std::string module, std::string name, Value value) {
            return state->update(module, [&](auto &m) {
                m.constants.push_back({std::move(name), std::move(value)});
            });
        },
        pure);
    library.bind(
        "Layout",
        [state](std::string module, std::string type) {
            return state->update(module, [&](auto &m) {
                RecordLayout item;
                item.c_type = std::move(type);
                m.layouts.push_back(std::move(item));
            });
        },
        pure);
    library.bind(
        "Field",
        [state](std::string module, std::string type, std::string field) {
            return state->update(module, [&](auto &m) {
                layout(m, type).fields.push_back({std::move(field), std::nullopt});
            });
        },
        pure);
    library.bind(
        "ExpectLayout",
        [state](std::string module, std::string type, std::size_t size, std::size_t alignment) {
            return state->update(module, [&](auto &m) {
                auto &item = layout(m, type);
                item.size = size;
                item.alignment = alignment;
            });
        },
        pure);
    library.bind(
        "ExpectOffset",
        [state](std::string module, std::string type, std::string field, std::size_t offset) {
            return state->update(module, [&](auto &m) {
                for (auto &item : layout(m, type).fields)
                    if (item.name == field) {
                        item.offset = offset;
                        return;
                    }
                throw std::invalid_argument("unknown record field: " + field);
            });
        },
        pure);
    library.bind(
        "Modules",
        [state] {
            std::vector<std::string> result;
            for (const auto &[name, module] : state->modules) {
                (void)module;
                result.push_back(name);
            }
            return result;
        },
        pure);
    library.bind(
        "Functions",
        [state](std::string module) {
            std::vector<std::string> result;
            for (const auto &f : state->get(module).functions)
                result.push_back(f.exported_name.empty() ? f.name : f.exported_name);
            return result;
        },
        pure);
    library.bind(
        "Headers", [state](std::string module) { return state->get(module).headers; }, pure);
    library.bind(
        "Validate", [state](std::string module) { return validate_binding(state->get(module)); },
        pure);
    library.bind(
        "Describe", [state](std::string module) { return describe_binding(state->get(module)); },
        pure);
    library.bind(
        "Generate", [state](std::string module) { return generate_bindings(state->get(module)); },
        pure);
    library.bind(
        "Check", [state](std::string module) { return generate_abi_checks(state->get(module)); },
        pure);
    library.bind(
        "Probe", [state](std::string module) { return generate_abi_probe(state->get(module)); },
        pure);

    StaticLibrary types("Types");
    types.bind(
             "Name", [](Value value) { return std::string(value_type(value)); }, pure)
        .bind(
            "String", [](Value value) { return stringify(value); }, pure)
        .bind(
            "IsNull", [](Value value) { return std::holds_alternative<std::monostate>(value); },
            pure)
        .bind(
            "Join",
            [](std::vector<std::string> values, std::string separator) {
                std::string result;
                for (std::size_t i = 0; i < values.size(); ++i) {
                    if (i)
                        result += separator;
                    result += values[i];
                }
                return result;
            },
            pure);
    // Stage both namespaces in a temporary registry to make installation atomic.
    Interpreter staging;
    library.install(staging);
    types.install(staging);
    interpreter.register_services(staging.services());
}
} // namespace moonstone::exoml
