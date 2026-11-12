#pragma once
#include "../../../core/SafetyEffects.hpp"
#include <regex>
namespace moonstone::exoml {
// A typed FFI declaration *model*, not a C parser or calling-convention engine.
// Feed validated declarations from cparser-iso11/ucpp via an external adapter.
struct CParameter {
    std::string name, c_type;
};
struct CFunction {
    std::string name, return_type;
    std::vector<CParameter> parameters;
    OwnershipContract ownership;
    bool variadic = false;
};
struct BindingModule {
    std::string name;
    std::vector<std::string> headers;
    std::vector<CFunction> functions;
};
inline std::vector<std::string> validate_binding(const BindingModule &m) {
    std::vector<std::string> errors;
    auto identifier = [](const std::string &s) {
        return !s.empty() && (std::isalpha((unsigned char)s[0]) || s[0] == '_') &&
               std::all_of(s.begin() + 1, s.end(),
                           [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    };
    if (!identifier(m.name))
        errors.push_back("invalid module name");
    std::set<std::string> names;
    for (auto &f : m.functions) {
        if (!identifier(f.name) || !names.insert(f.name).second)
            errors.push_back("invalid or duplicate function: " + f.name);
        if (f.return_type.empty())
            errors.push_back("missing return type: " + f.name);
        std::set<std::string> params;
        for (auto &p : f.parameters) {
            if (!identifier(p.name) || !params.insert(p.name).second || p.c_type.empty())
                errors.push_back("invalid parameter in " + f.name);
        }
        for (auto i : f.ownership.consumed_arguments)
            if (i >= f.parameters.size())
                errors.push_back("consumed index out of range: " + f.name);
        for (auto i : f.ownership.borrowed_arguments)
            if (i >= f.parameters.size())
                errors.push_back("borrowed index out of range: " + f.name);
        for (auto i : f.ownership.consumed_arguments)
            if (f.ownership.borrowed_arguments.count(i))
                errors.push_back("argument both consumed and borrowed: " + f.name);
    }
    return errors;
}
} // namespace moonstone::exoml
