#include "../internal.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#ifndef FLINTSTONE_STDLIB_DIR
#define FLINTSTONE_STDLIB_DIR "stdlib"
#endif

namespace flintstone {
namespace {

bool valid_library_name(std::string_view name) {
    if (!detail::valid_name(name)) return false;
    // Reject path traversal: valid_name allows '.', so forbid ".." explicitly.
    return name.find("..") == std::string_view::npos;
}

bool same_signature(const Function& a, const Function& b) {
    if (a.result != b.result || a.parameters.size() != b.parameters.size()) return false;
    for (std::size_t n = 0; n < a.parameters.size(); ++n)
        if (a.parameters[n].type != b.parameters[n].type) return false;
    return true;
}

// Apply the same contracts at every edge of the import graph. Copying only
// function bodies would lose dependency requirements and extern declarations.
//
// `host_is_final` distinguishes the real host module from an intermediate
// library. A dependency's required features travel upward with its code: an
// intermediate library accumulates them, and the real host must actually
// declare them. Checking a dependency against an intermediate library instead
// would reject a feature the host does provide.
void merge_library(Module& host, const Module& lib, const std::string& name, bool host_is_final) {
    detail::check(lib.target.name == host.target.name && lib.target.pointer_bits == host.target.pointer_bits &&
                      lib.target.index_bits == host.target.index_bits && lib.target.endian == host.target.endian,
                  "import.target", "imported library '" + name + "' target disagrees with host module");
    for (const auto& feature : lib.target.features) {
        if (host_is_final)
            detail::check(host.target.features.count(feature), "import.feature",
                          "imported library '" + name + "' requires missing target feature '" + feature + "'");
        else host.target.features.insert(feature);
    }
    detail::check(static_cast<unsigned>(lib.level) >= static_cast<unsigned>(host.level), "import.level",
                  "imported library '" + name + "' is at a higher IL level than host module");
    for (const auto& [dialect, params] : lib.dialects) {
        auto found = host.dialects.find(dialect);
        if (found == host.dialects.end()) host.dialects.emplace(dialect, params);
        else detail::check(found->second == params, "import.dialect",
                           "imported library '" + name + "' dialect '" + dialect + "' parameters conflict with host module");
    }
    for (const auto& fn : lib.functions) {
        bool merged = false;
        for (auto& existing : host.functions) {
            if (existing.name != fn.name) continue;
            detail::check(same_signature(existing, fn), "import.signature",
                          "imported symbol @" + fn.name + " signature conflicts with host declaration");
            detail::check(existing.external || fn.external, "import.duplicate",
                          "imported symbol @" + fn.name + " is already defined in host module");
            if (existing.external && !fn.external) existing = fn;
            merged = true;
            break;
        }
        if (!merged) host.functions.push_back(fn);
    }
}

std::string find_library_file(const std::string& name, const std::vector<std::string>& dirs) {
    const auto filename = name.size() > 6 && name.substr(name.size() - 6) == ".flint" ? name : name + ".flint";
    for (const auto& dir : dirs) {
        if (dir.empty()) continue;
        std::error_code ec;
        const auto candidate = std::filesystem::path(dir) / filename;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate.string();
    }
    detail::fail("import.not_found", "cannot find imported library '" + name + "'");
}

Module load_library(const std::string& name, const std::vector<std::string>& dirs, const Registry& registry,
                    std::set<std::string>& active, std::map<std::string, Module>& loaded, std::size_t depth) {
    detail::check(valid_library_name(name), "import.name", "invalid import library name '" + name + "'");
    detail::check(depth < 32, "import.depth", "import nesting limit exceeded");
    detail::check(active.insert(name).second, "import.cycle", "cyclic import of '" + name + "'");
    if (auto cached = loaded.find(name); cached != loaded.end()) {
        active.erase(name);
        return cached->second;
    }
    const auto path = find_library_file(name, dirs);
    std::ifstream file(path, std::ios::binary);
    detail::check(bool(file), "import.io", "cannot open imported library '" + path + "'");
    std::string source;
    char buffer[8192];
    while (file) {
        file.read(buffer, sizeof buffer);
        const auto count = static_cast<std::size_t>(file.gcount());
        detail::check(source.size() + count <= max_source_bytes, "import.limit", "imported library byte limit exceeded");
        source.append(buffer, count);
    }
    detail::check(!file.bad(), "import.io", "error reading imported library '" + path + "'");
    // Specialize in this module's namespace before comparing signatures or
    // importing types whose parameter names may mean something different here.
    auto imported = detail::specialize(parse(source, path));
    std::vector<std::string> children = std::move(imported.imports);
    imported.imports.clear();
    for (const auto& child : children) {
        auto dep = load_library(child, dirs, registry, active, loaded, depth + 1);
        merge_library(imported, dep, child, false);
    }
    imported = instantiate(imported, registry);
    active.erase(name);
    loaded.emplace(name, imported);
    return imported;
}

} // namespace

std::string default_stdlib_dir() {
    if (const char* env = std::getenv("FLINTSTONE_STDLIB_DIR"); env && *env) return env;
#if defined(__linux__) || defined(__APPLE__)
    // Locate the shared library via dladdr so relocated prefixes find
    // <prefix>/share/flintstone/stdlib without hard-coded install paths.
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&default_stdlib_dir), &info) != 0 && info.dli_fname) {
        std::error_code ec;
        const auto lib_path = std::filesystem::weakly_canonical(info.dli_fname, ec);
        if (!ec) {
            const auto candidate = lib_path.parent_path().parent_path() / "share" / "flintstone" / "stdlib";
            if (std::filesystem::is_directory(candidate, ec)) return candidate.string();
        }
    }
#endif
    return FLINTSTONE_STDLIB_DIR;
}

void resolve_imports(Module& module, const Registry& registry, const std::vector<std::string>& search_dirs) {
    if (module.imports.empty()) return;
    auto candidate = detail::specialize(module);
    std::vector<std::string> dirs = search_dirs;
    const auto fallback = default_stdlib_dir();
    if (!fallback.empty()) dirs.push_back(fallback);
    std::vector<std::string> pending = std::move(candidate.imports);
    candidate.imports.clear();
    std::set<std::string> active;
    std::map<std::string, Module> loaded;
    for (const auto& name : pending) {
        auto lib = load_library(name, dirs, registry, active, loaded, 0);
        merge_library(candidate, lib, name, true);
    }
    require_valid(candidate, registry);
    candidate.revision = module.revision + 1;
    module = std::move(candidate);
}

} // namespace flintstone
