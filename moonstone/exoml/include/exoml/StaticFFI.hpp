#pragma once
#include "AutoBind.hpp"

namespace moonstone::exoml {
// Validated declarations, not a lexical C parser or ABI-layout inference engine.
struct CParameter {
    std::string name, c_type;
};
struct CFunction {
    std::string name, return_type;
    std::vector<CParameter> parameters;
    OwnershipContract ownership;
    bool variadic = false;
    std::string exported_name;
    bool privileged = true;
    bool thread_affine = false;
    bool c_strings = false;
    std::string documentation;
    SourceSpan source;
};
struct BindingConstant {
    std::string name;
    Value value;
};
struct FieldLayout {
    std::string name;
    std::optional<std::size_t> offset;
};
struct RecordLayout {
    std::string c_type;
    std::optional<std::size_t> size, alignment;
    std::vector<FieldLayout> fields;
};
struct BindingModule {
    std::string name;
    std::vector<std::string> headers;
    std::vector<CFunction> functions;
    std::vector<BindingConstant> constants;
    std::vector<RecordLayout> layouts;
};

struct BindingLimits {
    std::size_t functions = 4096;
    std::size_t parameters = 128;
    std::size_t text_bytes = 1024 * 1024;
};
Diagnostics binding_diagnostics(const BindingModule &module, BindingLimits limits = {});
std::vector<std::string> validate_binding(const BindingModule &module);
// Deterministic JSON preserving ownership/effects and target expectations.
std::string describe_binding(const BindingModule &module);
// C++20 header exposing exoml_generated::bind_<module>(Interpreter&).
// Includes the real headers, asserts signatures/layout expectations, and
// installs the whole module atomically. Variadic/escaping bindings are rejected.
std::string generate_bindings(const BindingModule &module);
// Standalone C++20 translation unit: compile to verify signatures/layouts.
std::string generate_abi_checks(const BindingModule &module);
// Standalone C++20 program printing compiler-measured layout JSON.
std::string generate_abi_probe(const BindingModule &module);

// Shared state is retained by installed callbacks; builders need external
// synchronization. Services return declarations/source text, never execute it.
class StaticFFI {
    std::map<std::string, BindingModule> modules_;

  public:
    void add(BindingModule module);
    const BindingModule &module(const std::string &name) const;
    std::vector<std::string> modules() const;
    void install(Interpreter &interpreter) const;
};
// Installs an owned declaration workspace and pure Types/Reflection utilities.
// See stdlib/README.md for exact service signatures.
void install_static_ffi(Interpreter &interpreter);
} // namespace moonstone::exoml
