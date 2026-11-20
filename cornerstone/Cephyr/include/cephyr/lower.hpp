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

// Native MachineIR v1 plus the assembler names for definitions whose linkage
// is internal to the C translation unit.
struct LoweredUnit {
    std::string mir;
    std::vector<std::string> local_symbols;
    std::string data_assembly;
};

std::optional<LoweredUnit> lower(cparser::ASTNode *root, const Preprocessed &source,
                                 Diagnostics &diagnostics, bool hosted = true);

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

struct CType {
    enum class Kind { invalid, void_, integer, pointer, function, array, record };
    Kind kind = Kind::invalid;
    unsigned bits = 0, rank = 0;
    bool is_signed = false, is_plain_char = false, is_const = false, is_volatile = false;
    std::shared_ptr<const CType> element; // pointee, array element, or function result
    std::uint64_t count = 0;             // 0 for incomplete arrays
    std::vector<CType> params;           // adjusted function parameter types
    bool prototype = true, variadic = false;
    const void *record = nullptr;       // parser identity of an opaque record type

    bool valid() const { return kind != Kind::invalid; }
    bool integer() const { return kind == Kind::integer; }
    bool pointer() const { return kind == Kind::pointer; }
    bool function_pointer() const { return pointer() && element && element->kind == Kind::function; }
    bool scalar() const { return integer() || pointer(); }
};

struct Constant {
    CType type;
    std::uint64_t value = 0;
};

bool present(Node *);
std::vector<Node *> elements(Node *); // a list, a singleton, or an absent node
std::string identifier(Node *);
Location location(const Preprocessed &, Node *);
CType integer_type(unsigned bits = 32, bool is_signed = true, unsigned rank = 0);
CType plain_char_type(const Preprocessed &);
CType pointer_type(const CType &);
CType read_type(Node *, const Preprocessed &, Diagnostics &, bool allow_incomplete = false);
CType promote(const CType &);
CType common_integer(const CType &, const CType &);
bool compatible(const CType &, const CType &);
std::optional<std::uint64_t> size_of(const CType &);
unsigned align_of(const CType &);
std::string_view mir_type(const CType &);
std::uint64_t normalize(std::uint64_t, const CType &);
std::optional<Constant> integer_constant(Node *, const Preprocessed &, Diagnostics &);
std::optional<std::string> string_constant(Node *, const Preprocessed &, Diagnostics &);

struct Expr {
    CType type;
    Id id = no_value;
    // Only when evaluated as an ICE (or a single literal) for null conversions.
    std::optional<std::uint64_t> constant;
};

struct LValue {
    CType type;
    Id address = no_value;
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

    Unit(const Preprocessed &s, Diagnostics &d, bool h) : source(s), diagnostics(d), hosted(h) {}
    void error(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    void warn(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    std::string add_string(std::string bytes);
    std::string require_signature(const CType &);
    std::string require_variadic_call(const Symbol &, const std::vector<CType> &);
};

bool define_global(Unit &, Symbol &);
void check_static_assert(Unit &, Node *);

struct Function {
    Unit &unit;
    Symbol &symbol;
    std::vector<CType> values;
    std::vector<std::pair<unsigned, unsigned>> locals; // size, alignment
    std::vector<Block> blocks{Block{}};
    std::vector<Instruction> entry_addresses;
    std::vector<std::map<std::string, LValue>> scopes{{}};
    std::vector<std::pair<Id, Id>> loops; // break, continue (no_value if unavailable)
    Id current = 0;
    unsigned expression_depth = 0;

    Function(Unit &u, Symbol &s) : unit(u), symbol(s) {}
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
    Id emit(const CType &, std::string_view op, std::string operands = {});
    void emit_void(std::string_view op, std::string operands = {});
    Id constant(std::uint64_t, const CType & = integer_type());
    LValue local(const CType &, Node *at);
    std::optional<LValue> find_local(std::string_view) const;
    bool declare_local(std::string name, LValue, Node *);
    void error(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    void warn(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
};

// Function lowering and semantic conversions implemented across the frontend.
void lower_function_body(Function &, Node *parameters, Node *body);
void lower_stmt(Function &, Node *);
Expr lower_expr(Function &, Node *);
LValue lower_lvalue(Function &, Node *);
Expr load(Function &, LValue, Node *);
void store(Function &, LValue, Expr, Node *);
Expr convert(Function &, Expr, const CType &, Node *, bool explicit_cast = false);
Id truth(Function &, Expr, Node *);
// Canonical scalar argument word; includes the RV64 psABI's required extension.
Id abi_value(Function &, Expr);
std::vector<bool> reachable_blocks(const std::vector<Block> &);

} // namespace detail
} // namespace cephyr

#endif
