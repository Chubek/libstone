#pragma once

#include "Contracts.hpp"

namespace am8 {
namespace native_libraries {

// These adapters use checked-in public signatures, not ABI inference. They
// are emitted into the consumer source, never executed by the Amalgam8 compiler.
inline constexpr std::string_view xbind_adapter = R"AM8(
#ifdef __cplusplus
extern "C" inline
#else
static inline
#endif
int am8_xbind_has_target(const char *name) {
    return name != 0 && xbind_find_target(name) != 0;
}
)AM8";

inline constexpr std::string_view moonstone_adapter = R"AM8(
extern "C" inline int am8_moonstone_sexpr_arity(const char *text) {
    if (!text) return -1;
    // Read at most 4097 bytes of a valid NUL-terminated C string.
    constexpr std::size_t max_bytes = 4096;
    std::size_t size = 0;
    while (size <= max_bytes && text[size] != '\0') ++size;
    if (size > max_bytes) return -2;
    try {
        const auto node = moonstone::sexpr::Parser({text, size}, 512, 64).one();
        return node.list ? static_cast<int>(node.children.size()) : -1;
    } catch (const std::invalid_argument&) {
        return -1;
    } catch (const std::length_error&) {
        return -2;
    } catch (...) {
        return -3;
    }
}
)AM8";

struct Component {
    std::string_view family, name, header, target, conditions, adapter;
    Context context;
    bool available;
    bool compiled;
};

// `available` describes Amalgam8's named integration, not the set of
// host-provided CMake targets. Explicit legacy paths remain independent.
inline constexpr Component components[] = {
    {"tourchstone", "xbind", "xbind/xbind.h", "XBind::xbind",
     "TOURCHSTONE_BUILD_XBIND", xbind_adapter, Context::c, true, true},
    {"tourchstone", "xbind-qamrpp", "xbind/xbind_qamrpp.hpp", "XBind::QaMRppSpec",
     "TOURCHSTONE_BUILD_XBIND; XBIND_BUILD_DISCOVERY for binary discovery", {}, Context::cpp, true, true},
    {"tourchstone", "dynload", "dynload/dynload.h", "Tourchstone::dynload",
     "TOURCHSTONE_BUILD_DYNLOAD=ON and active Yellowstone qBFD", {}, Context::c, false, true},
    {"tourchstone", "dyninvoke", "dyninvoke/dyninvoke.h", "Tourchstone::dyninvoke",
     "TOURCHSTONE_BUILD_DYNINVOKE=ON, active Yellowstone qBFD and Sandstone ISA data",
     {}, Context::c, false, true},
    {"moonstone", "core", "core/SExpr.hpp", "Moonstone::Core", "C++20",
     moonstone_adapter, Context::cpp, true, false},
    {"moonstone", "chisel", "chisel/include/chisel/Chisel.hpp", "Moonstone::Chisel",
     "MOONSTONE_BUILD_CHISEL; C++20", {}, Context::cpp, true, false},
    {"moonstone", "syren", "syren/include/syren/Syren.hpp", "Moonstone::Syren",
     "MOONSTONE_BUILD_SYREN; C++20", {}, Context::cpp, true, false},
    {"moonstone", "supersonic", "supersonic/include/supersonic/Supersonic.hpp", "Moonstone::Supersonic",
     "MOONSTONE_BUILD_SUPERSONIC; C++20", {}, Context::cpp, true, false},
    {"moonstone", "exoml", "exoml/include/exoml/ExoML.hpp", "Moonstone::ExoML",
     "MOONSTONE_BUILD_EXOML; Perl and C++20", {}, Context::cpp, true, true},
    {"moonstone", "exoml-extract", "exoml/include/exoml/Extract.hpp", "Moonstone::ExoMLExtract",
     "MOONSTONE_EXOML_EXTRACT=ON, libclang and OpenSSL Crypto", {}, Context::cpp, false, true},
    {"moonstone", "exotract", "exotract/include/exotract/Exotract.hpp", "Moonstone::Exotract",
     "MOONSTONE_BUILD_EXOTRACT=ON", {}, Context::cpp, false, true},
    {"moonstone", "plaid", "plaid/Plaid.hpp", "Moonstone::Plaid",
     "MOONSTONE_BUILD_PLAID; C++20 and Threads", {}, Context::cpp, true, false},
    {"moonstone", "plaid-runtime", "plaid/runtime/PlaidRuntime.h", "Moonstone::PlaidRuntime",
     "MOONSTONE_BUILD_PLAID; C++ runtime and Threads", {}, Context::c, true, true},
    {"moonstone", "prolific", "prolific/Prolific.hpp", "Moonstone::Prolific",
     "MOONSTONE_BUILD_PROLIFIC=ON, vendored Lua/Kaguya/lmdbxx and LMDB", {}, Context::cpp, false, true},
    {"moonstone", "qmr", "qmr/include/qmr/Qmr.hpp", "Moonstone::Qmr",
     "MOONSTONE_BUILD_QMR=ON and vendored Lua", {}, Context::cpp, false, true},
    {"moonstone", "nerotypical", "nerotypical/include/nerotypical/sequent/SequentIR.hpp", "Nerotypical::Sequent",
     "MOONSTONE_BUILD_NEROTYPICAL; C++20", {}, Context::cpp, true, false},
};

} // namespace native_libraries

inline Dependency resolve_native(std::string_view family, std::string_view name,
                                 const Syntax& where, const Options&) {
    if (family != "tourchstone" && family != "moonstone")
        fail(where, "unknown native family '" + std::string(family) + "'");

    Dependency dep;
    dep.name = std::string(family) + ":" + std::string(name);
    dep.span = where.span;
    dep.include_roots = {"."}; // From the Libstone checkout, not the .am8 file.
    for (const auto& c : native_libraries::components) {
        if (c.family != family) continue;
        const auto include = c.header.find("/include/");
        const bool named = c.name == name;
        const bool header = c.header == name ||
            (include != std::string_view::npos &&
             c.header.substr(include + std::string_view("/include/").size()) == name);
        if (!named && !header) continue;
        if (named && !c.available) {
            fail(where, "named native dependency '" + dep.name +
                        "' is unavailable in Amalgam8; build and link " +
                        std::string(c.target) + " (" + std::string(c.conditions) +
                        "), then use its explicit header via (use " + std::string(family) +
                        " " + std::string(c.header) + ")");
        }
        dep.name = std::string(c.family) + ":" + std::string(c.name);
        dep.header = std::string(family) + "/" + std::string(c.header);
        dep.context = c.context;
        if (c.compiled) dep.libraries.emplace_back(c.target);
        dep.conditions = c.conditions;
        dep.adapter_source = c.adapter;
        return dep;
    }

    // Support historical family-relative header paths, even project-local
    // headers that were never part of the audited public inventory.
    if ((head(where) == "use" && where.items.size() == 3) ||
        name.find_first_of("./") != std::string_view::npos) {
        Syntax path;
        path.is_atom = true;
        path.text = name;
        path.span = where.span;
        path.provenance = where.provenance;
        dep.header = std::string(family) + "/" + include_path(path);
        dep.context = name.ends_with(".hpp") || name.ends_with(".hh") || name.ends_with(".hxx")
                          ? Context::cpp : Context::c;
        dep.conditions = "legacy path; provide include and link requirements explicitly";
        return dep;
    }
    fail(where, "unknown native dependency '" + dep.name +
                "'; use a catalog component or an explicit family-relative header");
}

} // namespace am8
