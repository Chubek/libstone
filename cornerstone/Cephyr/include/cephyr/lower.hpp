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
    // Retain the type qualifier without assuming no-alias (§6.7.3.1p6).
    bool is_restrict = false;
    std::shared_ptr<const CType> element; // pointee, array element, or function result
    std::uint64_t count = 0;             // 0 for incomplete arrays
    std::vector<CType> params;           // adjusted function parameter types
    bool prototype = true, variadic = false;
    const RecordLayout *record = nullptr; // stable within the translation unit
    const void *enum_identity = nullptr;   // parser tag/definition identity; integer representation
    // Expression-only metadata: the declared type still determines size and
    // compatibility. The restricted range changes integer promotions.
    unsigned bit_field_width = 0;
    bool denotes_bit_field = false;

    bool valid() const { return kind != Kind::invalid; }
    bool integer() const { return kind == Kind::integer; }
    bool real() const { return kind == Kind::real; }
    bool extended_real() const { return real() && bits == 80; }
    bool arithmetic() const { return integer() || real(); }
    bool pointer() const { return kind == Kind::pointer; }
    bool aggregate() const { return kind == Kind::record || kind == Kind::array; }
    bool function_pointer() const { return pointer() && element && element->kind == Kind::function; }
    bool scalar() const { return arithmetic() || pointer(); }
    void clear_qualifiers() { is_const = is_volatile = is_restrict = false; }
};

struct BitField {
    unsigned width;
    unsigned shift; // position within the first byte of the field
    unsigned byte_in_container; // offset from the aligned base-type container
    enum class VolatileAccess { unsupported, narrow, container };
    VolatileAccess volatile_access;

    std::uint64_t mask() const { return (std::uint64_t{1} << width) - 1; }
    unsigned bytes() const { return (shift + width + 7) / 8; }
};

struct RecordMember {
    std::string name; // empty for a physical anonymous structure or union
    CType type;
    std::uint64_t offset;
    // Unnamed fields are represented in the layout but not initialized.
    std::optional<BitField> bit_field = std::nullopt;

    bool flexible_array() const { return type.kind == CType::Kind::array && !type.count; }
    bool unnamed_bit_field() const { return bit_field && name.empty(); }
};

struct RecordLayout {
    bool is_union = false;
    bool defining = false;
    bool has_const_member = false;
    bool has_volatile_member = false;
    // C11 6.7.2.1p3 also restricts unions that recursively contain such a structure.
    bool contains_flexible_array = false;
    bool contains_anonymous_member = false; // recursively through value members, not pointers
    bool contains_bit_field = false; // recursively through value members, not pointers
    std::uint64_t size = 0; // zero while the type is incomplete
    unsigned alignment = 0;
    std::vector<RecordMember> members;
    // Visible names map to physical paths through (possibly nested) anonymous
    // records. Storage traversal must still use 'members', not these aliases.
    std::map<std::string, std::vector<std::uint64_t>, std::less<>> member_paths;
};

struct MemberAccess {
    CType type;
    std::uint64_t offset;
    std::optional<BitField> bit_field = std::nullopt;
};

struct Constant {
    CType type;
    std::uint64_t value = 0;
    // The upper 16 value bits of an x87 extended constant. All other
    // constants are fully represented in value.
    std::uint16_t x87_sign_exponent = 0;
};

struct Unit;
struct Function;
struct Symbol;
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
    TypeBinding lookup(std::string_view, Node *reference = nullptr) const;

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
CType read_type(Node *, const TypeContext &, bool allow_incomplete = false,
                bool function_definition = false);
CType read_parameter_type(Node *, const TypeContext &, bool definition);
CType read_record_type(Node *, const TypeContext &);
std::optional<MemberAccess> find_member(const CType &, std::string_view);
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
// All bundled LP64 targets support these fundamental alignments in every
// object context. Extended alignments are not currently supported.
inline constexpr unsigned max_object_alignment = 16;
std::optional<unsigned> alignment_specifiers(Node *, const TypeContext &);
bool check_object_alignment(unsigned requested, unsigned natural, Node *, const TypeContext &);
std::string_view mir_type(const CType &);
std::uint64_t normalize(std::uint64_t, const CType &);
std::optional<Constant> integer_constant(Node *, const TypeContext &);
std::optional<Constant> arithmetic_initializer_constant(Node *, const CType &, const TypeContext &);
std::optional<Constant> real_constant(Node *, const TypeContext &, bool require_native = true);
std::optional<Constant> layout_constant(Node *, const TypeContext &);
CType string_constant_type(Node *, const Preprocessed &);
std::string string_constant_bytes(Node *); // target representation, including terminator

struct Expr {
    CType type;
    // Native pointer for a record value, low 64-bit payload for x87.
    Id id = no_value;
    // Only when evaluated as an ICE (or a single literal) for null conversions.
    std::optional<std::uint64_t> constant;
    Id x87_sign_exponent = no_value; // separate integer SSA value, not an ABI slot
};

struct LValue {
    CType type;
    Id address = no_value;
    std::string static_name = {}; // assembly name for a block-scope static object
    bool register_object = false;
    Symbol *linked = nullptr; // block-scope declaration of a linked entity
    // For supported volatile bit-fields the address is the aligned base-type
    // container; nonvolatile bit-fields use the first byte they occupy.
    std::optional<BitField> bit_field = std::nullopt;
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
    // Keep the selected definition's specifier separate from the type and
    // from other declarations. A present zero is distinct from no specifier.
    std::optional<unsigned> definition_alignment = std::nullopt;
    std::vector<std::pair<unsigned, Node *>> alignment_declarations = {};
    // A block-only declaration contributes linkage but not file-scope visibility.
    bool file_scope = false;
    CType linkage_type = {}; // reconcile otherwise hidden declarations
    bool inline_only = false;
    Node *inline_declaration = nullptr;
    Node *inline_internal_reference = nullptr;
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
    std::map<std::string, CType> rv64_soft_fp_helpers;
    std::map<std::size_t, std::string> record_pair_calls;
    std::map<std::string, std::vector<std::size_t>> variadic_signatures;
    std::vector<Global> globals;
    std::vector<std::string> locals;
    std::string data_assembly;
    std::map<std::pair<unsigned, std::string>, std::string> strings; // alignment, bytes -> name
    std::map<const Node *, std::pair<CType, std::string>> static_compound_literals;
    // The parser shares bound expression nodes when it clones typedef types.
    // Resolve each bound where the declarator first appears, not at a later use.
    // Zero denotes a checked nonconstant bound erased by parameter adjustment.
    std::map<const Node *, std::optional<std::uint64_t>> array_bounds;
    // Multiple declarators share the specifiers and their original scope.
    std::map<const Node *, std::optional<unsigned>> alignment_values;
    std::map<const void *, RecordLayout> record_layouts; // keyed by parser tag identity
    std::map<const Node *, std::uint64_t> enumeration_constants; // keyed by defining enumerator
    std::size_t block_statics = 0;
    std::size_t function_names = 0;
    std::size_t compound_literals = 0;

    Unit(const Preprocessed &s, Diagnostics &d, bool h) : source(s), diagnostics(d), hosted(h) {}
    Symbol *find_file_symbol(std::string_view);
    void error(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    void warn(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    std::string add_string(Node *literal, const CType &type);
    std::optional<std::pair<CType, std::string>> add_static_compound_literal(Node *);
    void require_function_name(const std::string &symbol, std::string_view spelling);
    std::string require_signature(const CType &);
    std::string require_variadic_call(const Symbol &, const std::vector<CType> &);
    std::string require_record_pair_call(const CType &);
    std::string add_record_pair_definition(const Symbol &);
};

bool define_global(Unit &, Symbol &, const Function *scope = nullptr);
void check_static_assert(const TypeContext &, Node *);

// Subobject initializers. Ordinary plans assume zero-filled storage; a
// volatile automatic plan explicitly lists the omitted subobjects to zero.
// String entries carry the exact bytes, including any required terminator.
struct ObjectInitEntry {
    std::uint64_t offset;
    CType type;
    Node *source;
    std::optional<std::string> bytes;
    bool clear = false;
    std::optional<BitField> bit_field = std::nullopt;
};

bool object_contains_bit_field(const CType &);

std::optional<std::vector<ObjectInitEntry>> plan_object_initializer(
    const TypeContext &, CType &, Node *, bool direct = false);

// Compound literals use declaration initializer rules but have no C name.
std::optional<std::vector<ObjectInitEntry>> plan_compound_literal(
    const TypeContext &, Node *, CType &, bool evaluated = false);
bool validate_static_compound_initializer(const TypeContext &, const std::vector<ObjectInitEntry> &);
LValue lower_compound_literal(Function &, Node *);

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
    std::map<const Node *, LValue> compound_literal_slots;
    std::vector<std::pair<Id, Id>> loops; // break, continue (no_value if unavailable)
    // Labels have their own function-wide namespace (C11 6.2.1, 6.2.3).
    std::map<std::string, Label> labels;
    Id current = 0;
    Id va_start_parameter = no_value; // rightmost eligible named parameter's C local
    Id record_result_address = no_value;
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
    LValue local(const CType &, Node *at, unsigned requested_alignment = 0);
    std::optional<LValue> find_local(std::string_view) const;
    std::optional<LValue> find_linked(std::string_view, Node *reference = nullptr) const;
    bool declare_local(std::string name, LValue, Node *);
    void require_static(const LValue &) const;
    void error(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
    void warn(Node *, std::string_view message, std::string_view code = "cephyr.semantic");
};

void declare_block_linkage(Function &, Node *declaration, Node *name, CType, unsigned flags,
                           std::optional<unsigned> alignment = std::nullopt);

// Function lowering and semantic conversions implemented across the frontend.
void lower_function_body(Function &, Node *parameters, Node *body);
Expr lower_expr(Function &, Node *);
LValue lower_lvalue(Function &, Node *);
Expr load(Function &, LValue, Node *);
void store(Function &, LValue, Expr, Node *);
bool copy_record(Function &, LValue, Expr, Node *, bool initializing = false);
Id offset_address(Function &, Id, std::uint64_t, const CType &);
void zero_object(Function &, Id, std::uint64_t size, unsigned alignment);
void zero_initialized_subobject(Function &, Id, const CType &);
Expr convert(Function &, Expr, const CType &, Node *, bool explicit_cast = false);
Id truth(Function &, Expr, Node *);
// Canonical scalar argument word; includes the RV64 psABI's required extension.
Id abi_value(Function &, Expr);
// Keep the C signature intact for type checking; classify and marshal only
// at a native call boundary. Record values themselves remain address-backed.
std::string_view record_abi_error(const Preprocessed &, const CType &function_type);
enum class RecordABI { scalar, word, pair, indirect };
RecordABI record_abi(const CType &);
CType abi_result_type(const CType &);
std::vector<CType> abi_parameter_types(const CType &signature, bool pair_body = false);
Id pack_record_abi(Function &, Expr, unsigned word = 0);
void unpack_record_abi(Function &, LValue, Id value, unsigned word = 0);
CType record_pair_buffer_type();
bool append_abi_argument(Function &, Expr, Node *, std::vector<Id> &);
std::vector<bool> reachable_blocks(const std::vector<Block> &);

} // namespace detail
} // namespace cephyr

#endif
