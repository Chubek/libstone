#pragma once

#include "Catalog.hpp"
#include "Emitter.hpp"
#include "Expansion.hpp"
#include "Frontend.hpp"
#include "Native.hpp"
#include "NativeLibraries.hpp"
#include "Semantic.hpp"

namespace am8 {
namespace detail {

// Dependencies have their own graph, separate from the source module graph.
// Walk it once, recording entries in prerequisites-first order.
class Dependencies {
    const Options& options_;
    std::vector<Dependency> ordered_;
    std::map<std::string, int, std::less<>> state_;

    void use(std::string_view family, std::string_view item, const Syntax& at) {
        if (family != "firestone" && family != "tourchstone" && family != "moonstone")
            fail(at, "unknown standard-library family '" + std::string(family) + "'");
        auto dep = family == "firestone" ? resolve_firestone(item, at, options_)
                                       : resolve_native(family, item, at, options_);
        if (state_[dep.name] == 2) return;
        if (state_[dep.name] == 1) fail(at, "cyclic native dependency '" + dep.name + "'");
        state_[dep.name] = 1;
        for (const auto& needed : dep.requires_names) qualified(needed, at);
        state_[dep.name] = 2;
        ordered_.push_back(std::move(dep));
    }

    void qualified(std::string_view name, const Syntax& at) {
        const auto colon = name.find(':');
        if (colon == std::string_view::npos || colon == 0 || colon + 1 == name.size())
            fail(at, "expected a family-qualified dependency, e.g. firestone:BigInt");
        use(name.substr(0, colon), name.substr(colon + 1), at);
    }

public:
    explicit Dependencies(const Options& options) : options_(options) {}
    std::vector<Dependency> collect(const Program& program) {
        for (const auto& module : program.modules) {
            for (const auto& form : module.forms) {
                if (head(form) == "include") {
                    auto path = include_path(form.items.at(1));
                    if (state_["system:" + path] == 2) continue;
                    state_["system:" + path] = 2;
                    Dependency dep;
                    dep.name = dep.header = std::move(path);
                    dep.kind = DependencyKind::system;
                    dep.span = form.span;
                    ordered_.push_back(std::move(dep));
                } else if (head(form) == "use") {
                    if (form.items.size() == 3)
                        use(identifier(form.items[1]), include_path(form.items[2]), form);
                    else
                        qualified(atom(form.items.at(1)), form);
                }
            }
        }
        return std::move(ordered_);
    }
};

inline std::vector<OpaqueFragment> describe_opaque(const Unit& unit) {
    std::vector<OpaqueFragment> fragments;
    const auto expression = [&](auto&& self, const Expr& expr) -> void {
        if (expr.kind == ExprKind::opaque)
            fragments.push_back({Context::c, "expression", expr.span});
        for (const auto& child : expr.children) self(self, child);
    };
    const auto statement = [&](auto&& self, const Stmt& stmt) -> void {
        if (stmt.kind == StmtKind::opaque)
            fragments.push_back({Context::c, "statement", stmt.span});
        for (const auto& expr : stmt.expressions) expression(expression, expr);
        for (const auto& child : stmt.children) self(self, child);
    };
    for (const auto& decl : unit.declarations) {
        if (decl.kind == DeclKind::raw_cpp || decl.kind == DeclKind::raw_c)
            fragments.push_back({decl.kind == DeclKind::raw_cpp ? Context::cpp : Context::c,
                                 "declaration", decl.span});
        if (decl.initializer) expression(expression, *decl.initializer);
        if (decl.body) statement(statement, *decl.body);
        for (const auto& item : decl.enumerators)
            if (item.second) expression(expression, *item.second);
    }
    if (unit.entry && unit.entry->raw)
        fragments.push_back({Context::cpp, "entry", unit.entry->span});
    return fragments;
}
} // namespace detail

inline Result compile(Program program, const Options& options = {}) {
    if (program.modules.empty() || program.root >= program.modules.size())
        throw Error("invalid Amalgam8 program: missing root module");
    for (std::size_t i = 0; i < program.modules.size(); ++i)
        for (const auto& dependency : program.modules[i].imports)
            if (dependency.target >= i)
                fail(dependency.span, "invalid program: modules must be in dependency-first order");
    expand(program, options.native, options.limits);
    auto unit = lower(program, detail::Dependencies(options).collect(program));
    return {emit(unit, options.mode), std::move(unit.dependencies), detail::describe_opaque(unit)};
}

// base is an import directory, not a file name. Diagnostics use filename.
inline Result compile_source(std::string_view source, std::string filename = "<input>",
                             const Options& options = {}, const fs::path& base = ".") {
    auto syntax = parse(source, std::move(filename), options.limits);
    auto load_limits = options.limits;
    load_limits.source_bytes -= source.size() - syntax.span.end;
    return compile(load(std::move(syntax), base, load_limits), options);
}

inline Result compile_file(const fs::path& input, const Options& options = {}) {
    return compile(load(input, options.limits), options);
}

} // namespace am8
