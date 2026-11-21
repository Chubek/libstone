#ifndef CEPHYR_LOWER_HPP
#define CEPHYR_LOWER_HPP

#include <cephyr/diagnostics.hpp>

#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cparser { class ASTNode; }

namespace cephyr {

struct Preprocessed;
namespace detail { class MoonstoneAnalysis; }

// Native MachineIR v1 plus the assembler names for definitions whose linkage
// is internal to the C translation unit.
struct LoweredUnit {
    std::string mir;
    std::vector<std::string> local_symbols;
    std::string data_assembly;
};

std::optional<LoweredUnit> lower(cparser::ASTNode *root, const Preprocessed &source,
                                 Diagnostics &diagnostics, bool hosted = true,
                                 detail::MoonstoneAnalysis *analysis = nullptr);

namespace detail {

using Node = cparser::ASTNode;
using Id = unsigned;
inline constexpr Id no_value = std::numeric_limits<Id>::max();

class AnalysisDepth {
public:
    explicit AnalysisDepth(unsigned &depth) : depth_(depth)
    {
        if (depth_ >= 512)
            throw std::length_error("C semantic nesting limit exceeded");
        ++depth_;
    }
    ~AnalysisDepth() { --depth_; }
    AnalysisDepth(const AnalysisDepth &) = delete;
    AnalysisDepth &operator=(const AnalysisDepth &) = delete;
private:
    unsigned &depth_;
};

struct RecordLayout;

struct CType {
    enum class Kind { invalid, void_, integer, real, pointer, function, array, record };
    Kind kind = Kind::invalid;
    unsigned bits = 0, rank = 0;
    bool is_signed = false, is_plain_char = false, is_const = false, is_volatile = false;
    std::shared_ptr<const CType> element; // pointee, array element, or function result
    std::uint64_t count = 0;             // 0 for incomplete arrays
    std::vector<CType> params;           // adjusted function parameter types
    bool prototype = true, variadic = false;
    const RecordLayout *record = nullptr; // stable within the translation unit

    bool valid() const { return kind != Kind::invalid; }
    bool integer() const { return kind == Kind::integer; }
    bool real() const { return kind == Kind::real; }
    bool arithmetic() const { return integer() || real(); }
    bool pointer() const { return kind == Kind::pointer; }
    bool aggregate() const { return kind == Kind::record || kind == Kind::array; }
    bool function_pointer() const { return pointer() && element && element->kind == Kind::function; }
    bool scalar() const { return arithmetic() || pointer(); }
};

struct RecordMember {
    std::string name;
    CType type;
    std::uint64_t offset;
};

struct RecordLayout {
    bool is_union = false;
    bool defining = false;
    bool has_const_member = false;
    bool has_volatile_member = false;
    std::uint64_t size = 0; // zero while the type is incomplete
    unsigned alignment = 0;
    std::vector<RecordMember> members;
};

struct Constant {
    CType type;
    std::uint64_t value = 0;
};

struct Unit;
struct Function;
struct TypeBinding {
    CType type;
    bool register_object = false;
};
struct PrototypeScope {
    const PrototypeScope *parent;
    std::map<std::string, TypeBinding> parameters;
};

struct TypeContext {
    Unit &unit;
    const Preprocessed &source;
    Diagnostics &diagnostics;
    const Function *function;
    const PrototypeScope *prototype = nullptr;
    bool speculative = false;

    TypeContext(Unit &, const Function * = nullptr);
    TypeContext with_probe(Diagnostics &) const;
    TypeBinding lookup(std::string_view) const;

private:
    TypeContext(Unit &u, const Preprocessed &s, Diagnostics &d, const Function *f,
                const PrototypeScope *p, bool probe)
        : unit(u), source(s), diagnostics(d), function(f), prototype(p), speculative(probe) {}
};

bool present(Node *);
std::vector<Node *> elements(Node *); // a list, a singleton, or an absent node
std::string identifier(Node *);
Location location(const Preprocessed &, Node *);
CType integer_type(unsigned bits = 32, bool is_signed = true, unsigned rank = 0);
CType real_type(unsigned bits);
CType plain_char_type(const Preprocessed &);
CType pointer_type(const CType &);
CType read_type(Node *, const TypeContext &, bool allow_incomplete = false);
CType read_record_type(Node *, const TypeContext &);
const RecordMember *find_member(const CType &, std::string_view);
CType qualified_member_type(const CType &, const RecordMember &);
bool object_has_qualifier(const CType &, bool CType::*qualifier);
void bind_typedef_bounds(Node *, const TypeContext &);
CType expression_type(Node *, const TypeContext &);
// Check a generic selection and return only its result expression, or nullptr
// on error. Optionally return the result's type before any value conversions.
Node *select_generic(Node *, const TypeContext &, CType *result_type = nullptr);
CType promote(const CType &);
CType common_integer(const CType &, const CType &);
CType common_arithmetic(const CType &, const CType &);
bool compatible(const CType &, const CType &);
std::optional<std::uint64_t> size_of(const CType &);
unsigned align_of(const CType &);
std::string_view mir_type(const CType &);
std::uint64_t normalize(std::uint64_t, const CType &);
std::optional<Constant> integer_constant(Node *, const TypeContext &);
std::optional<Constant> real_constant(Node *, const TypeContext &, bool require_native = true);
std::optional<Constant> size_or_alignment(Node *, const TypeContext &);
std::optional<std::string> string_constant(Node *, const Preprocessed &, Diagnostics &);

struct Expr {
    CType type;
    Id id = no_value; // native pointer to the representation for a record value
    // Only when evaluated as an ICE (or a single literal) for null conversions.
    std::optional<std::uint64_t> constant;
};

struct LValue {
    CType type;
    Id address = no_value;
    std::string static_name = {}; // assembly name for a block-scope static object
    bool register_object = false;
};

struct Edge {
    Id target;
    std::vector<Id> args;
};

struct Term {
    enum class Kind { open, ret, jump, branch, trap };
    Kind kind = Kind::open;
    Id value = no_value;
    Edge yes{no_value, {}}, no{no_value, {}};
};

struct Instruction {
    std::string opcode, operands;
    Id result = no_value;
    Node *origin = nullptr;
};

struct Block {
    std::vector<Id> params;
    std::vector<Instruction> instructions;
    Term term;
};

struct Symbol {
    CType type;
    std::string name; // assembler name
    Node *declaration = nullptr;
    Node *definition = nullptr;
    bool internal = false;
    bool used = false;
    bool initialized = false;
};

struct Global {
    std::string name;
    std::size_t words = 0; // zero declares an import
    std::vector<std::uint64_t> data; // empty for a zero initializer
};

struct Unit {
    const Preprocessed &source;
    Diagnostics &diagnostics;
    bool hosted = true;
    std::map<std::string, Symbol> symbols;
    std::vector<std::pair<std::string, CType>> signatures;
    std::map<std::string, std::vector<std::size_t>> variadic_signatures;
    std::vector<Global> globals;
    std::vector<std::string> locals;
    std::string data_assembly;
    std::map<std::string, std::string> strings; // literal bytes -> assembler name
    // The parser shares bound expression nodes when it clones typedef types.
    // Resolve each bound where the declarator first appears, not at a later use.
    std::map<const Node *, std::optional<std::uint64_t>> array_bounds;
    std::map<const void *, RecordLayout> record_layouts; // keyed by parser tag identity
    std::size_t block_statics = 0;
    std::size_t function_names = 0;

    Unit(const Preprocessed &s, Diagnostics &d, bool h) : source(s), diagnostics(d), hosted(h) {}
    void error(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    void warn(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    std::string add_string(std::string bytes);
    void require_function_name(const std::string &symbol, std::string_view spelling);
    std::string require_signature(const CType &);
    std::string require_variadic_call(const Symbol &, const std::vector<CType> &);
};

bool define_global(Unit &, Symbol &, const Function *scope = nullptr);
void check_static_assert(const TypeContext &, Node *);

// Explicit subobject initializers; the object starts zero-filled. String
// entries contain the exact bytes to copy, including any required terminator.
struct ObjectInitEntry {
    std::uint64_t offset;
    CType type;
    Node *source;
    std::optional<std::string> bytes;
    bool clear = false;
};

std::optional<std::vector<ObjectInitEntry>> plan_object_initializer(
    const TypeContext &, CType &, Node *);

struct Function {
    struct Label {
        Id target = no_value;
        Node *definition = nullptr;
        Node *first_reference = nullptr;
    };

    Unit &unit;
    Symbol &symbol;
    std::string source_name;
    std::string function_name_symbol;
    std::vector<CType> values;
    std::vector<std::pair<unsigned, unsigned>> locals; // size, alignment
    std::vector<Block> blocks{Block{}};
    std::vector<Instruction> entry_addresses;
    std::vector<std::map<std::string, LValue>> scopes{{}};
    std::vector<std::pair<Id, Id>> loops; // break, continue (no_value if unavailable)
    // Labels have their own function-wide namespace (C11 6.2.1, 6.2.3).
    std::map<std::string, Label> labels;
    Id current = 0;
    unsigned expression_depth = 0, statement_depth = 0;

    Function(Unit &u, Symbol &s, std::string name) : unit(u), symbol(s), source_name(std::move(name)) {}
    bool open() const;
    Id block();
    void select(Id);
    Id param(Id block, const CType &);
    void jump(Id, std::vector<Id> args = {});
    void branch(Id cond, Id yes, Id no, std::vector<Id> yes_args = {},
                std::vector<Id> no_args = {});
    void ret(Id = no_value);
    void trap();
    Id value(const CType &);
    Id emit(const CType &, std::string_view op, std::string operands = {}, Node *origin = nullptr);
    void emit_void(std::string_view op, std::string operands = {});
    Id constant(std::uint64_t, const CType & = integer_type());
    LValue local(const CType &, Node *at);
    std::optional<LValue> find_local(std::string_view) const;
    bool declare_local(std::string name, LValue, Node *);
    void require_static(const LValue &) const;
    void error(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    void warn(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
};

// Function lowering and semantic conversions implemented across the frontend.
void lower_function_body(Function &, Node *parameters, Node *body);
Expr lower_expr(Function &, Node *);
LValue lower_lvalue(Function &, Node *);
Expr load(Function &, LValue, Node *);
void store(Function &, LValue, Expr, Node *);
bool copy_record(Function &, LValue, Expr, Node *, bool initializing = false);
Id offset_address(Function &, Id, std::uint64_t, const CType &);
void zero_object(Function &, Id, std::uint64_t size, unsigned alignment);
Expr convert(Function &, Expr, const CType &, Node *, bool explicit_cast = false);
Id truth(Function &, Expr, Node *);
// Canonical scalar argument word; includes the RV64 psABI's required extension.
Id abi_value(Function &, Expr);
std::vector<bool> reachable_blocks(const std::vector<Block> &);

} // namespace detail
} // namespace cephyr

#endif
