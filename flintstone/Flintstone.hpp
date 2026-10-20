#pragma once

#include <libir/SimpleSSAGraph.hpp>
#include <libvm/Environment.hpp>
#include <libvm/Module.hpp>
#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flintstone {

using ValueId = std::uint32_t;
using BlockId = std::uint32_t;
inline constexpr ValueId invalid_id = UINT32_MAX;

struct Location {
    std::string file;
    std::size_t line = 0, column = 0;
};
struct Diagnostic {
    std::string code, message;
    Location location;
    std::string function;
    BlockId block = invalid_id;
    std::size_t operation = SIZE_MAX;
};
using Diagnostics = std::vector<Diagnostic>;
std::string format(const Diagnostic&);
class Error : public std::runtime_error {
public:
    explicit Error(Diagnostics);
    Error(std::string code, std::string message, Location = {});
    const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
private:
    Diagnostics diagnostics_;
};

enum class Level { High, Core, Machine };
const char* level_name(Level);
enum class TypeKind { Void, Integer, Float, Index, Pointer, Vector, Tuple, Opaque, Parameter };
struct Type {
    TypeKind kind = TypeKind::Void;
    unsigned bits = 0;
    std::uint32_t count = 0;
    std::string name;
    std::vector<Type> elements;
    static Type void_type();
    static Type integer(unsigned bits = 32);
    static Type floating(unsigned bits = 64);
    static Type index();
    static Type pointer(std::string address_space = "generic");
    static Type vector(std::uint32_t count, Type element);
    static Type tuple(std::vector<Type>);
    static Type opaque(std::string name, std::vector<Type> parameters = {});
    static Type parameter(std::string name);
    bool operator==(const Type&) const = default;
};
std::string print(const Type&);
using Attribute = std::variant<std::int64_t, double, bool, std::string, Type>;
using Attributes = std::map<std::string, Attribute>;

enum class Endian { Little, Big };
struct Target {
    std::string name = "vm";
    unsigned pointer_bits = 64, index_bits = 64;
    Endian endian = Endian::Little;
    std::set<std::string> features;
    Attributes parameters;
};
struct Layout { std::uint64_t size = 0, alignment = 1; };
// Scalar/aggregate layout is explicit; opaque types require a dialect callback.
Layout layout(const Type&, const Target&);

struct Value {
    ValueId id = invalid_id;
    Type type;
    Location location;
};
enum class OperandKind { Value, Integer, Float, Symbol };
struct Operand {
    OperandKind kind = OperandKind::Value;
    ValueId value = invalid_id;
    std::int64_t integer = 0;
    double floating = 0;
    std::string symbol;
    Type type;
    static Operand ref(ValueId);
    static Operand constant(std::int64_t, Type = Type::integer());
    static Operand real(double, Type = Type::floating());
    static Operand named(std::string, Type);
};
struct Operation {
    std::string name;
    std::vector<Value> results;
    std::vector<Operand> operands;
    std::vector<BlockId> successors, incoming;
    Attributes attributes;
    Location location;
};
struct Block {
    BlockId id = invalid_id;
    std::string name;
    std::vector<Operation> operations;
    Location location;
};
struct Function {
    std::string name;
    std::vector<Value> parameters;
    Type result;
    std::vector<Block> blocks;
    BlockId entry = invalid_id;
    bool external = false;
    Attributes attributes;
    Location location;
    ValueId fresh_value() const;
    BlockId fresh_block() const;
    Block& block(BlockId);
    const Block& block(BlockId) const;
};
struct Module {
    std::string name = "module";
    Level level = Level::High;
    Target target;
    // Unresolved library names; resolve_imports() links them into functions
    // and clears this list. verify() rejects any remaining unresolved imports.
    std::vector<std::string> imports;
    std::map<std::string, Type> type_bindings;
    std::map<std::string, Attributes> dialects;
    Attributes attributes;
    std::vector<Function> functions;
    std::uint64_t revision = 0;
    Function& function(const std::string&);
    const Function& function(const std::string&) const;
};

class Builder {
public:
    explicit Builder(Function& function) : function_(function) {}
    Value parameter(Type);
    BlockId block(std::string name = {});
    Value emit(BlockId, std::string name, Type, std::vector<Operand>, Attributes = {});
    void effect(BlockId, std::string name, std::vector<Operand> = {}, Attributes = {});
    void jump(BlockId from, BlockId to);
    void branch(BlockId from, Operand condition, BlockId yes, BlockId no);
    void return_value(BlockId, std::optional<Operand> = {});
private:
    Function& function_;
};

enum class Effect { Pure, Read, Write, Unknown, Control };
class Registry;
struct OperationContext {
    const Module& module;
    const Function& function;
    const Registry& registry;
    const std::map<ValueId, Type>& values;
    Attributes dialect_parameters;
    Type type(const Operand&) const;
};
struct OperationDefinition {
    std::string name;
    Level level = Level::High;
    std::size_t min_operands = 0, max_operands = 0;
    std::size_t min_results = 0, max_results = 0;
    Effect effect = Effect::Unknown;
    bool terminator = false, phi = false;
    // Optional, explicit semantic bridge; unknown operations are never guessed.
    std::optional<firestone::ir::Opcode> ssa_opcode;
    std::function<void(const Operation&, const OperationContext&)> verify;
};
struct DialectDefinition {
    std::string name;
    unsigned version = 1;
    Attributes parameters;
    std::vector<OperationDefinition> operations;
    std::function<void(const Attributes&, const Target&)> verify_parameters;
    std::function<bool(const Type&, const Attributes&, const Target&)> verify_type;
};
struct LoweringRule {
    std::string name, operation;
    Level destination = Level::Core;
    // The replacement owns its results; preserve the original public result IDs.
    std::function<std::vector<Operation>(const Operation&, const Function&, const Module&, const Registry&)> rewrite;
};
struct Artifact {
    std::string format;
    std::vector<std::uint8_t> bytes;
    struct Relocation { std::size_t offset; std::string kind, symbol; std::int64_t addend; };
    std::vector<Relocation> relocations;
    struct Symbol { std::string name; std::size_t offset, size; };
    std::vector<Symbol> symbols;
};
using Backend = std::function<Artifact(const Module&, const Registry&)>;
class Registry {
public:
    Registry(); // Builtin arith, cf, func, mem, math, vm, vector, tuple, ptr and atomic dialects.
    void add_dialect(DialectDefinition);
    void add_lowering(LoweringRule);
    void add_backend(std::string name, Backend);
    const OperationDefinition& operation(const std::string&) const;
    const DialectDefinition& dialect(const std::string&) const;
    Attributes parameters(const Module&, const std::string&) const;
    const std::vector<LoweringRule>& lowerings() const { return lowerings_; }
    const Backend& backend(const std::string&) const;
    std::vector<std::string> dialect_names() const;
    std::vector<std::string> backend_names() const;
private:
    std::map<std::string, DialectDefinition> dialects_;
    std::map<std::string, OperationDefinition> operations_;
    std::vector<LoweringRule> lowerings_;
    std::map<std::string, Backend> backends_;
};

struct ParseLimits { std::size_t bytes = 16 * 1024 * 1024, tokens = 1000000, type_depth = 64; };
Module parse(std::string_view source, std::string filename = "<input>", const ParseLimits& = {});
std::string print(const Module&);
Diagnostics verify(const Module&, const Registry&);
void require_valid(const Module&, const Registry&);
// Resolve `import "name";` declarations against search directories (defaulting
// to the standard library directory). Rejects path traversal, cycles, target
// or level mismatches, and duplicate definitions; replaces matching externs.
std::string default_stdlib_dir();
void resolve_imports(Module&, const Registry&, const std::vector<std::string>& search_dirs = {});
// Substitution is cycle/occurs checked. Index types become target-width integers.
Module instantiate(const Module&, const Registry&);
// Transactional, bounded, verified conversion. No implicit backend fallback.
void lower(Module&, const Registry&, Level destination, std::size_t rewrite_limit = 100000);
Artifact emit(const Module&, const Registry&, const std::string& backend);

// The function reference must belong to the supplied module.
firestone::ir::SimpleSSAGraph to_ssa(const Module&, const Function&, const Registry&);
firestone::vm::Module compile_vm(const Module&, const Registry&,
                                firestone::vm::VMModel model = firestone::vm::VMModel::Register);
// Install the declared modular-integer intrinsics in a Firestone environment.
// Required when executing emitted FSVM directly; execute() does this for you.
void bind_vm_runtime(firestone::vm::Environment&, const firestone::vm::Module&);
firestone::vm::Value execute(const Module&, const Registry&, const std::string& function,
                            const std::vector<firestone::vm::Value>& arguments = {},
                            std::size_t instruction_budget = 1000000,
                            firestone::vm::Environment* environment = nullptr);

struct DefUse {
    struct Use { std::string function; BlockId block; std::size_t operation, operand; };
    std::map<std::pair<std::string, ValueId>, std::vector<Use>> uses;
};
// Analyses and passes are host-extensible. A manager is single-use per run.
class AnalysisManager {
public:
    using Factory = std::function<std::any(const Module&, const Registry&)>;
    AnalysisManager();
    void add(std::string name, Factory);
    const std::any& get(const std::string&, const Module&, const Registry&);
    void invalidate(const std::set<std::string>& preserved);
private:
    std::map<std::string, Factory> factories_;
    std::map<std::string, std::any> cache_;
};
struct Pass {
    std::string name;
    std::set<std::string> requirements, preserves;
    // A preservation claim is a host contract: a pass that mutates the CFG
    // but lists "cfg" as preserved causes later passes to reuse stale analyses.
    // The driver verifies validity and change reporting, not preservation truth.
    std::function<bool(Module&, const Registry&, AnalysisManager&)> run;
};
struct PassReport { std::string name; bool changed; std::set<std::string> preserved; };
class PassManager {
public:
    void add(Pass);
    void add_analysis(std::string name, AnalysisManager::Factory);
    std::vector<PassReport> run(Module&, const Registry&) const;
private:
    std::vector<Pass> passes_;
    std::map<std::string, AnalysisManager::Factory> analyses_;
};
Pass dead_code_elimination(); // Keeps reads, calls and potentially trapping ops.
Pass blackstone_pass(std::string kernel_directory, std::vector<std::string> kernels);
// Unroll vector packs to scalar arithmetic and eliminate tuple aggregates via
// scalar replacement. Both fail explicitly when a value escapes the function
// (parameters, results, calls, stores, returns) instead of changing signatures.
Pass vector_scalarize_pass();
Pass tuple_sroa_pass();

// Tourchstone/XBind target discovery and binding-spec resolution.
std::vector<std::string> binding_targets();
std::string binding_spec(const std::string& directory);
std::string version();

} // namespace flintstone
