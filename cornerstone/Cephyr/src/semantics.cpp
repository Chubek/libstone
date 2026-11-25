#include <cephyr/lower.hpp>

#include "declarations.hpp"
#include "floating_arithmetic.hpp"

#include <cephyr/preprocess.hpp>

#include <AbstractSyntaxTree.h>
#include <ASTNode.h>
#include <wetstone/abstract-interp/TypedInt.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cephyr::detail {
namespace {

using wetstone::ai::TypedInt;

constexpr unsigned bool_rank = 0;
constexpr unsigned char_rank = 1;
constexpr unsigned short_rank = 2;
constexpr unsigned int_rank = 3;
constexpr unsigned long_rank = 4;
constexpr unsigned long_long_rank = 5;

constexpr unsigned qualifier_mask = DS_QUALIFIER_MASK;

std::uint64_t bit_mask(unsigned bits) {
    if (bits == 0 || bits > 64)
        return 0;
    return bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                      : (std::uint64_t{1} << bits) - 1;
}

unsigned default_rank(unsigned bits) {
    if (bits <= 1)
        return bool_rank;
    if (bits <= 8)
        return char_rank;
    if (bits <= 16)
        return short_rank;
    if (bits <= 32)
        return int_rank;
    return long_rank;
}

template <class T>
std::string_view spelling(const T *node) {
    return node->getSpelling();
}

void type_error(Node *node, const Preprocessed &source, Diagnostics &diagnostics,
                std::string_view message) {
    diagnostics.error("cephyr.type", message, location(source, node));
}

void constant_error(Node *node, const Preprocessed &source, Diagnostics &diagnostics,
                    std::string_view message) {
    diagnostics.error("cephyr.constant", message, location(source, node));
}

CType unqualified(CType type) {
    type.clear_qualifiers();
    return type;
}

CType qualify(CType type, unsigned qualifiers) {
    type.is_const = (qualifiers & TQ_CONST) != 0;
    type.is_volatile = (qualifiers & TQ_VOLATILE) != 0;
    type.is_restrict = (qualifiers & TQ_RESTRICT) != 0;
    type.is_atomic = (qualifiers & TQ_ATOMIC) != 0;
    return type;
}

CType adjust_parameter(CType type) {
    if (type.kind == CType::Kind::array && type.element) {
        // C11 applies qualifiers on an array typedef to its element type.
        CType element = *type.element;
        element.is_const |= type.is_const;
        element.is_volatile |= type.is_volatile;
        element.is_restrict |= type.is_restrict;
        return pointer_type(element);
    }
    if (type.kind == CType::Kind::function)
        return pointer_type(type);
    return unqualified(std::move(type));
}

bool tagged_record_equal(const CType &left, const CType &right) {
    // A tag spelling is not an identity: the same spelling can denote distinct
    // records in nested scopes. The parser or TU symbol layer must intern
    // repeated declarations of one tag before they reach CType.
    return left.record != nullptr && left.record == right.record;
}

bool signed_value(const Constant &constant, std::int64_t &result) {
    if (!constant.type.integer() || !constant.type.is_signed)
        return false;
    result = std::bit_cast<std::int64_t>(normalize(constant.value, constant.type));
    return true;
}

std::optional<std::uint64_t> positive_count(const Constant &constant) {
    if (!constant.type.integer())
        return std::nullopt;
    if (constant.type.is_signed) {
        std::int64_t value;
        if (!signed_value(constant, value) || value < 0)
            return std::nullopt;
        return static_cast<std::uint64_t>(value);
    }
    return normalize(constant.value, constant.type);
}

Constant converted(Constant value, CType type) {
    type = unqualified(std::move(type));
    value.value = normalize(value.value, type);
    value.type = std::move(type);
    return value;
}

struct ConstantAnalysis {
    std::map<Node *, Constant> sizes;
    std::map<Node *, Node *> selections;
    // Populated only for §6.6p7–8 initializer evaluation. Do not use these
    // forms to recognize an ICE (or indirectly a null pointer constant).
    std::map<Node *, Constant> real_literals;
    std::map<Node *, CType> initializer_types;
};
std::optional<CType> infer_integer_type(Node *, const TypeContext &, const ConstantAnalysis &);
std::optional<Constant> evaluate_constant(Node *, const TypeContext &, const ConstantAnalysis &);

thread_local unsigned semantic_depth = 0;

struct GenericResolution {
    Node *selected;
    CType type;
};

// Expression typing may ask more than once about the same selection, e.g.
// when testing an address operand's type, lvalue status, and storage class.
// Cache successful checks for the duration of one selection only, never
// across declaration points or speculative diagnostic contexts.
struct GenericAnalysis {
    const TypeContext &context;
    std::map<Node *, GenericResolution> resolved;
};

thread_local GenericAnalysis *generic_analysis = nullptr;

bool digit_value(char character, unsigned &digit) {
    if (character >= '0' && character <= '9')
        digit = static_cast<unsigned>(character - '0');
    else if (character >= 'a' && character <= 'f')
        digit = static_cast<unsigned>(character - 'a' + 10);
    else if (character >= 'A' && character <= 'F')
        digit = static_cast<unsigned>(character - 'A' + 10);
    else
        return false;
    return true;
}

struct LiteralSuffix {
    bool unsigned_ = false;
    unsigned longs = 0;
};

std::optional<LiteralSuffix> parse_suffix(std::string_view suffix) {
    LiteralSuffix result;
    std::size_t index = 0;
    while (index < suffix.size()) {
        const char current =
            static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[index])));
        if (current == 'u' && !result.unsigned_) {
            result.unsigned_ = true;
            ++index;
            continue;
        }
        if (current == 'l' && result.longs == 0) {
            result.longs = 1;
            const char first = suffix[index];
            ++index;
            if (index < suffix.size() && suffix[index] == first) {
                result.longs = 2;
                ++index;
            }
            continue;
        }
        return std::nullopt;
    }
    return result;
}

struct IntegerCandidate {
    unsigned bits;
    bool signed_;
    unsigned rank;
};

bool fits_literal(std::uint64_t value, const IntegerCandidate &candidate) {
    if (!candidate.signed_)
        return candidate.bits == 64 || value <= bit_mask(candidate.bits);
    if (candidate.bits == 64)
        return value <= static_cast<std::uint64_t>(
                            std::numeric_limits<std::int64_t>::max());
    return value <= (std::uint64_t{1} << (candidate.bits - 1)) - 1;
}

std::vector<IntegerCandidate> literal_candidates(bool decimal,
                                                 const LiteralSuffix &suffix) {
    const IntegerCandidate int_ = {32, true, int_rank};
    const IntegerCandidate uint_ = {32, false, int_rank};
    const IntegerCandidate long_ = {64, true, long_rank};
    const IntegerCandidate ulong_ = {64, false, long_rank};
    const IntegerCandidate long_long = {64, true, long_long_rank};
    const IntegerCandidate ulong_long = {64, false, long_long_rank};

    if (decimal) {
        if (suffix.unsigned_ && suffix.longs == 0)
            return {uint_, ulong_, ulong_long};
        if (suffix.unsigned_ && suffix.longs == 1)
            return {ulong_, ulong_long};
        if (suffix.unsigned_ && suffix.longs == 2)
            return {ulong_long};
        if (suffix.longs == 1)
            return {long_, long_long};
        if (suffix.longs == 2)
            return {long_long};
        return {int_, long_, long_long};
    }

    if (suffix.unsigned_ && suffix.longs == 0)
        return {uint_, ulong_, ulong_long};
    if (suffix.unsigned_ && suffix.longs == 1)
        return {ulong_, ulong_long};
    if (suffix.unsigned_ && suffix.longs == 2)
        return {ulong_long};
    if (suffix.longs == 1)
        return {long_, ulong_, long_long, ulong_long};
    if (suffix.longs == 2)
        return {long_long, ulong_long};
    return {int_, uint_, long_, ulong_, long_long, ulong_long};
}

std::optional<Constant> parse_integer_spelling(
    Node *node, std::string_view text, const Preprocessed &source,
    Diagnostics &diagnostics) {
    if (text.empty()) {
        constant_error(node, source, diagnostics, "empty integer literal");
        return std::nullopt;
    }

    unsigned radix = 10;
    std::size_t index = 0;
    bool decimal = true;
    if (text.size() >= 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        radix = 16;
        index = 2;
        decimal = false;
    } else if (text.size() > 1 && text[0] == '0') {
        radix = 8;
        index = 0;
        decimal = false;
    }

    const std::size_t digits_begin = index;
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        unsigned digit = 0;
        if (!digit_value(text[index], digit))
            break;
        if (digit >= radix) {
            constant_error(node, source, diagnostics,
                           "digit is invalid for the integer literal base");
            return std::nullopt;
        }
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / radix) {
            constant_error(node, source, diagnostics,
                           "integer literal exceeds 64 bits");
            return std::nullopt;
        }
        value = value * radix + digit;
    }
    if (index == digits_begin) {
        constant_error(node, source, diagnostics,
                       "integer literal has no digits");
        return std::nullopt;
    }

    const auto suffix = parse_suffix(text.substr(index));
    if (!suffix) {
        constant_error(node, source, diagnostics,
                       "invalid integer literal suffix");
        return std::nullopt;
    }
    for (const auto candidate : literal_candidates(decimal, *suffix)) {
        if (fits_literal(value, candidate)) {
            CType type =
                integer_type(candidate.bits, candidate.signed_, candidate.rank);
            return Constant{type, normalize(value, type)};
        }
    }
    constant_error(node, source, diagnostics,
                   "integer literal is not representable by a supported C type");
    return std::nullopt;
}

std::optional<Constant> integer_token_value(Node *node, const TypeContext &context) {
    if (!present(node) || node->getKind() != cparser::NK_INTEGER_CONST)
        return std::nullopt;
    auto *literal = static_cast<cparser::IntegerConstASTNode *>(node);
    if (const auto *enumerator = literal->getEnumerator()) {
        const auto &values = context.unit.enumeration_constants;
        const auto found = values.find(enumerator);
        if (found != values.end())
            return Constant{integer_type(), found->second};
        constant_error(node, context.source, context.diagnostics,
                       "no constant value for enumerator '" +
                       std::string(enumerator->getName()) + "'");
        return std::nullopt;
    }
    const auto original = spelling(literal);
    if (!original.empty())
        return parse_integer_spelling(node, original, context.source, context.diagnostics);
    CType type = integer_type();
    const auto value = literal->getValue();
    if (normalize(value, type) != value) {
        constant_error(node, context.source, context.diagnostics,
                       "integer constant without token spelling does not fit in int");
        return std::nullopt;
    }
    return Constant{type, value};
}

bool evaluate_enumerators(Node *body, const TypeContext &context) {
    if (!present(body))
        return true;
    const auto &members = static_cast<cparser::SequenceASTNode *>(body)->getElements();
    auto &values = context.unit.enumeration_constants;
    if (members.empty() || values.contains(members.back()))
        return true;

    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    // Use a widened value so even INT_MAX + 1 can be diagnosed without
    // overflowing. Explicit initializers reset the sequence (6.7.2.2p2-3).
    std::int64_t next = 0;
    for (Node *member : members) {
        auto *enumerator = static_cast<cparser::EnumeratorDeclASTNode *>(member);
        const auto diagnose = [&](std::string_view suffix) {
            constant_error(member, context.source, context.diagnostics,
                           "enumerator '" + std::string(enumerator->getName()) + "' " +
                           std::string(suffix));
        };
        if (Node *initializer = enumerator->getInitializer(); present(initializer)) {
            const auto errors = context.diagnostics.error_count();
            const auto constant = integer_constant(initializer, context);
            if (!constant) {
                if (context.diagnostics.error_count() == errors)
                    diagnose("requires an integer constant expression");
                return false;
            }
            if (constant->type.is_signed) {
                (void)signed_value(*constant, next);
            } else if (constant->value <= static_cast<std::uint64_t>(maximum)) {
                next = static_cast<std::int64_t>(constant->value);
            } else {
                next = std::int64_t{maximum} + 1;
            }
        }
        if (next < minimum || next > maximum) {
            diagnose("has a value not representable as int");
            return false;
        }
        values.emplace(member, static_cast<std::uint64_t>(next));
        ++next;
    }
    return true;
}

CType character_literal_type(const cparser::CharConstASTNode *literal,
                             const Preprocessed &source) {
    const auto text = spelling(literal);
    if (!text.empty()) {
        switch (text.front()) {
        case 'L': return integer_type(32, source.wchar_signed);
        case 'u': return integer_type(16, false);
        case 'U': return integer_type(32, false);
        default: break;
        }
    }
    return integer_type();
}

Constant character_literal(Node *node, const Preprocessed &source) {
    const auto *literal = static_cast<cparser::CharConstASTNode *>(node);
    const auto type = character_literal_type(literal, source);
    std::uint64_t bits = literal->getValue();
    // 6.4.4.4p10: only an ordinary single-byte constant converts from char
    // to int; a packed multibyte value is interpreted directly as int.
    if (literal->isSingleByte() && source.plain_char_signed && bits >= 128)
        bits -= 256;
    return Constant{type, normalize(bits, type)};
}

std::optional<std::pair<Node *, Node *>> binary_operands(Node *node) {
    switch (node->getKind()) {
#define CEPHYR_BINARY_CASE(kind, type)                                         \
    case kind: {                                                               \
        auto *binary = static_cast<type *>(node);                              \
        return std::pair<Node *, Node *>{binary->getLhs(), binary->getRhs()};  \
    }
        CEPHYR_BINARY_CASE(cparser::NK_LSHIFT_EXPR, cparser::LShiftExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_RSHIFT_EXPR, cparser::RShiftExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_BIT_IOR_EXPR, cparser::BitIorExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_BIT_XOR_EXPR, cparser::BitXorExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_BIT_AND_EXPR, cparser::BitAndExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_LOG_AND_EXPR, cparser::LogAndExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_LOG_OR_EXPR, cparser::LogOrExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_PLUS_EXPR, cparser::PlusExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_MINUS_EXPR, cparser::MinusExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_MULT_EXPR, cparser::MultExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_TRUNC_DIV_EXPR,
                           cparser::TruncDivExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_TRUNC_MOD_EXPR,
                           cparser::TruncModExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_LT_EXPR, cparser::LtExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_LE_EXPR, cparser::LeExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_GT_EXPR, cparser::GtExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_GE_EXPR, cparser::GeExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_EQ_EXPR, cparser::EqExprASTNode)
        CEPHYR_BINARY_CASE(cparser::NK_NE_EXPR, cparser::NeExprASTNode)
#undef CEPHYR_BINARY_CASE
    default:
        return std::nullopt;
    }
}

// Check the form separately from computing its value. A skipped operand may
// contain an otherwise undefined arithmetic operation, but a variable or call
// there still prevents the expression from being an integer constant expression.
bool has_ice_form(Node *node, const TypeContext &context, ConstantAnalysis &analysis) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node))
        return false;
    switch (node->getKind()) {
    case cparser::NK_INTEGER_CONST:
    case cparser::NK_CHAR_CONST:
        return true;
    case cparser::NK_SIZEOF_EXPR:
    case cparser::NK_ALIGNOF_EXPR:
    case cparser::NK_OFFSETOF_EXPR: {
        // Validate the types even in an unselected conditional arm. Cache
        // the result so the value evaluator need not inspect the operand again.
        const auto size = layout_constant(node, context);
        if (!size)
            return false;
        analysis.sizes.emplace(node, *size);
        return true;
    }
    case cparser::NK_GENERIC_EXPR: {
        // C11 6.5.1.1: only the result expression supplies the type and value.
        // The controller and other associations still have to be well-typed.
        auto found = analysis.selections.find(node);
        Node *selected = found != analysis.selections.end()
            ? found->second : select_generic(node, context, nullptr);
        if (!selected)
            return false;
        analysis.selections.emplace(node, selected);
        return has_ice_form(selected, context, analysis);
    }
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        auto *type = cast->getType();
        if (!present(type) || !present(cast->getExpr()))
            return false;
        return (type->getKind() == cparser::NK_BOOLEAN_TYPE ||
                type->getKind() == cparser::NK_INTEGRAL_TYPE ||
                type->getKind() == cparser::NK_ENUMERAL_TYPE ||
                type->getKind() == cparser::NK_LIST) &&
               read_type(type, context).integer() &&
               (cast->getExpr()->getKind() == cparser::NK_REAL_CONST ||
                 has_ice_form(cast->getExpr(), context, analysis));
    }
    case cparser::NK_UNARY_PLUS_EXPR:
        return has_ice_form(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr(), context, analysis);
    case cparser::NK_BIT_NOT_EXPR:
        return has_ice_form(static_cast<cparser::BitNotExprASTNode *>(node)->getExpr(), context, analysis);
    case cparser::NK_LOG_NOT_EXPR:
        return has_ice_form(static_cast<cparser::LogNotExprASTNode *>(node)->getExpr(), context, analysis);
    case cparser::NK_COND_EXPR: {
        auto *cond = static_cast<cparser::CondExprASTNode *>(node);
        return has_ice_form(cond->getCondition(), context, analysis) &&
               has_ice_form(cond->getThenClause(), context, analysis) &&
               has_ice_form(cond->getElseClause(), context, analysis);
    }
    default:
        const auto operands = binary_operands(node);
        return operands && has_ice_form(operands->first, context, analysis) &&
               has_ice_form(operands->second, context, analysis);
    }
}

FloatingLiteral decode_real_literal(Node *node, const Preprocessed &source, Diagnostics &diagnostics) {
    const auto literal = parse_floating_literal(
        spelling(static_cast<cparser::RealConstASTNode *>(node)), source.native_amd64_x87, source.native_binary128);
    std::string_view error;
    switch (literal.status) {
    case FloatingLiteralStatus::ok:
        return literal;
    case FloatingLiteralStatus::unsupported_long_double:
        error = "long double floating constants are not supported";
        break;
    case FloatingLiteralStatus::overflow:
        error = "floating constant is out of range for its nominal format";
        break;
    case FloatingLiteralStatus::too_long:
        error = "floating constant exceeds the spelling limit";
        break;
    case FloatingLiteralStatus::malformed:
        error = "malformed floating constant";
        break;
    }
    constant_error(node, source, diagnostics, error);
    return literal;
}

// C11 6.6p6 permits a floating constant as the immediate operand of an
// integer cast, even when floating-point arithmetic is not lowered natively.
std::optional<Constant> cast_real_literal(Node *node, CType destination,
                                         const Preprocessed &source,
                                         Diagnostics &diagnostics) {
    const auto literal = decode_real_literal(node, source, diagnostics);
    if (!literal)
        return std::nullopt;
    const auto bits = floating_literal_to_integer(literal, destination.bits, destination.is_signed);
    if (!bits) {
        constant_error(node, source, diagnostics,
                       "floating constant is not representable in the cast's integer type");
        return std::nullopt;
    }
    return Constant{unqualified(destination), normalize(*bits, destination)};
}

CType analyze_arithmetic_initializer(Node *node, const TypeContext &context, ConstantAnalysis &analysis) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node))
        return {};
    switch (node->getKind()) {
    case cparser::NK_REAL_CONST: {
        const auto value = real_constant(node, context);
        if (!value)
            return {};
        analysis.real_literals.emplace(node, *value);
        return value->type;
    }
    case cparser::NK_SIZEOF_EXPR:
    case cparser::NK_ALIGNOF_EXPR:
    case cparser::NK_OFFSETOF_EXPR: {
        const auto value = layout_constant(node, context);
        if (!value)
            return {};
        analysis.sizes.emplace(node, *value);
        return value->type;
    }
    case cparser::NK_GENERIC_EXPR: {
        auto *selected = select_generic(node, context);
        if (!selected)
            return {};
        analysis.selections.emplace(node, selected);
        return analyze_arithmetic_initializer(selected, context, analysis);
    }
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        const auto target = unqualified(read_type(cast->getType(), context, true));
        if (!target.arithmetic() || !present(cast->getExpr()))
            return {};
        // A direct literal-to-integer cast already works without an FP ABI.
        if (target.integer() && cast->getExpr()->getKind() == cparser::NK_REAL_CONST) {
            const auto value = real_constant(cast->getExpr(), context, false);
            if (!value)
                return {};
            analysis.real_literals.emplace(cast->getExpr(), *value);
        } else if (!analyze_arithmetic_initializer(cast->getExpr(), context, analysis).arithmetic()) {
            return {};
        }
        analysis.initializer_types.emplace(node, target);
        return target;
    }
    case cparser::NK_UNARY_PLUS_EXPR: {
        const auto type = analyze_arithmetic_initializer(
            static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr(), context, analysis);
        return type.real() ? type : promote(type);
    }
    case cparser::NK_BIT_NOT_EXPR:
        return promote(analyze_arithmetic_initializer(
            static_cast<cparser::BitNotExprASTNode *>(node)->getExpr(), context, analysis));
    case cparser::NK_LOG_NOT_EXPR:
        return analyze_arithmetic_initializer(
            static_cast<cparser::LogNotExprASTNode *>(node)->getExpr(), context, analysis).arithmetic()
            ? integer_type() : CType{};
    case cparser::NK_MINUS_EXPR: {
        auto *minus = static_cast<cparser::MinusExprASTNode *>(node);
        if (!minus->isUnary())
            break;
        const auto type = analyze_arithmetic_initializer(minus->getRhs(), context, analysis);
        if (type.real()) {
            analysis.initializer_types.emplace(node, type);
            return type;
        }
        return promote(type);
    }
    case cparser::NK_COND_EXPR: {
        auto *cond = static_cast<cparser::CondExprASTNode *>(node);
        if (!analyze_arithmetic_initializer(cond->getCondition(), context, analysis).arithmetic())
            return {};
        const auto yes = analyze_arithmetic_initializer(cond->getThenClause(), context, analysis);
        const auto no = analyze_arithmetic_initializer(cond->getElseClause(), context, analysis);
        const auto result = common_arithmetic(yes, no);
        if (result.valid())
            analysis.initializer_types.emplace(node, result);
        return result;
    }
    default:
        break;
    }
    const auto operands = binary_operands(node);
    if (!operands) {
        // Check types of forms such as calls and comma expressions even in
        // unselected arms. The evaluator cannot produce a value for them
        // (§6.6p3); only short-circuit/unevaluated placement can admit them.
        return expression_type(node, context);
    }
    const auto lhs = analyze_arithmetic_initializer(operands->first, context, analysis);
    const auto rhs = analyze_arithmetic_initializer(operands->second, context, analysis);
    switch (node->getKind()) {
    case cparser::NK_LSHIFT_EXPR:
    case cparser::NK_RSHIFT_EXPR:
        return rhs.integer() ? promote(lhs) : CType{};
    case cparser::NK_BIT_IOR_EXPR:
    case cparser::NK_BIT_XOR_EXPR:
    case cparser::NK_BIT_AND_EXPR:
    case cparser::NK_TRUNC_MOD_EXPR:
        return common_integer(lhs, rhs);
    case cparser::NK_LOG_AND_EXPR:
    case cparser::NK_LOG_OR_EXPR:
    case cparser::NK_LT_EXPR:
    case cparser::NK_LE_EXPR:
    case cparser::NK_GT_EXPR:
    case cparser::NK_GE_EXPR:
    case cparser::NK_EQ_EXPR:
    case cparser::NK_NE_EXPR:
        return lhs.arithmetic() && rhs.arithmetic() ? integer_type() : CType{};
    default:
        return common_arithmetic(lhs, rhs);
    }
}

std::optional<CType> infer_integer_type(Node *node, const TypeContext &context,
                                        const ConstantAnalysis &analysis) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node))
        return std::nullopt;
    switch (node->getKind()) {
    case cparser::NK_INTEGER_CONST: {
        const auto literal = integer_token_value(node, context);
        return literal ? std::optional<CType>(literal->type) : std::nullopt;
    }
    case cparser::NK_CHAR_CONST:
        return character_literal_type(static_cast<cparser::CharConstASTNode *>(node),
                                      context.source);
    case cparser::NK_GENERIC_EXPR:
        return infer_integer_type(analysis.selections.at(node), context, analysis);
    case cparser::NK_UNARY_PLUS_EXPR: {
        auto operand = infer_integer_type(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr(),
                                          context, analysis);
        return operand ? std::optional<CType>(promote(*operand)) : std::nullopt;
    }
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        CType type = read_type(cast->getType(), context);
        return type.integer() ? std::optional<CType>(unqualified(type))
                              : std::nullopt;
    }
    case cparser::NK_BIT_NOT_EXPR: {
        auto *unary = static_cast<cparser::BitNotExprASTNode *>(node);
        const auto operand = infer_integer_type(unary->getExpr(), context, analysis);
        return operand ? std::optional<CType>(promote(*operand)) : std::nullopt;
    }
    case cparser::NK_LOG_NOT_EXPR:
    case cparser::NK_LOG_AND_EXPR:
    case cparser::NK_LOG_OR_EXPR:
    case cparser::NK_LT_EXPR:
    case cparser::NK_LE_EXPR:
    case cparser::NK_GT_EXPR:
    case cparser::NK_GE_EXPR:
    case cparser::NK_EQ_EXPR:
    case cparser::NK_NE_EXPR:
        return integer_type();
    case cparser::NK_LSHIFT_EXPR:
    case cparser::NK_RSHIFT_EXPR: {
        const auto operands = binary_operands(node);
        const auto left =
            operands
                ? infer_integer_type(operands->first, context, analysis)
                : std::nullopt;
        return left ? std::optional<CType>(promote(*left)) : std::nullopt;
    }
    case cparser::NK_BIT_IOR_EXPR:
    case cparser::NK_BIT_XOR_EXPR:
    case cparser::NK_BIT_AND_EXPR:
    case cparser::NK_PLUS_EXPR:
    case cparser::NK_MINUS_EXPR:
    case cparser::NK_MULT_EXPR:
    case cparser::NK_TRUNC_DIV_EXPR:
    case cparser::NK_TRUNC_MOD_EXPR: {
        const auto operands = binary_operands(node);
        if (!operands)
            return std::nullopt;
        const auto left =
            infer_integer_type(operands->first, context, analysis);
        const auto right =
            infer_integer_type(operands->second, context, analysis);
        if (!left || !right)
            return std::nullopt;
        return common_integer(*left, *right);
    }
    case cparser::NK_COND_EXPR: {
        auto *conditional = static_cast<cparser::CondExprASTNode *>(node);
        const auto yes =
            infer_integer_type(conditional->getThenClause(), context, analysis);
        const auto no =
            infer_integer_type(conditional->getElseClause(), context, analysis);
        if (!yes || !no)
            return std::nullopt;
        return common_integer(*yes, *no);
    }
    case cparser::NK_SIZEOF_EXPR:
    case cparser::NK_ALIGNOF_EXPR:
    case cparser::NK_OFFSETOF_EXPR:
        return integer_type(64, false, long_rank);
    default:
        return std::nullopt;
    }
}

bool true_value(const Constant &constant) {
    if (constant.type.real() && constant.type.bits == 128)
        return constant.value || (constant.high_word << 1);
    if (constant.type.extended_real())
        return constant.value != 0;
    if (constant.type.real())
        return (constant.value & bit_mask(constant.type.bits - 1)) != 0;
    return normalize(constant.value, constant.type) != 0;
}

FloatingLiteral real_bits(const Constant &value) {
    return {FloatingLiteralStatus::ok, value.type.bits, value.value, value.high_word};
}

std::optional<Constant> convert_arithmetic_constant(
    Node *node, Constant value, CType type, const TypeContext &context) {
    if (type.real()) {
        const auto result = value.type.real()
            ? (type.extended_real() || value.type.extended_real()
                ? convert_x87_literal(real_bits(value), type.bits)
                : convert_floating_literal(real_bits(value), type.bits))
            : integer_to_floating_literal(normalize(value.value, value.type),
                                          value.type.is_signed, type.bits);
        if (result)
            return Constant{unqualified(type), result.bits, result.high_word};
    } else if (value.type.real()) {
        if (const auto bits = floating_literal_to_integer(real_bits(value), type.bits, type.is_signed))
            return converted(Constant{type, *bits}, type);
    } else {
        return converted(value, type);
    }
    constant_error(node, context.source, context.diagnostics,
                   "arithmetic constant conversion is out of range for its destination type");
    return std::nullopt;
}

std::optional<Constant> evaluate_floating_binary(
    Node *node, const Constant &left, const Constant &right, const TypeContext &context) {
    const CType common = common_arithmetic(left.type, right.type);
    const auto lhs = convert_arithmetic_constant(node, left, common, context);
    const auto rhs = convert_arithmetic_constant(node, right, common, context);
    if (!lhs || !rhs)
        return std::nullopt;
    const auto a = real_bits(*lhs);
    const auto b = real_bits(*rhs);
    const auto kind = node->getKind();
    if (kind >= cparser::NK_LT_EXPR && kind <= cparser::NK_NE_EXPR) {
        const int comparison = compare_finite_floating(a, b);
        const bool result = kind == cparser::NK_LT_EXPR ? comparison < 0
                          : kind == cparser::NK_LE_EXPR ? comparison <= 0
                          : kind == cparser::NK_GT_EXPR ? comparison > 0
                          : kind == cparser::NK_GE_EXPR ? comparison >= 0
                          : kind == cparser::NK_EQ_EXPR ? comparison == 0
                                                       : comparison != 0;
        return Constant{integer_type(), result};
    }
    FloatingOperation operation;
    switch (kind) {
    case cparser::NK_PLUS_EXPR: operation = FloatingOperation::add; break;
    case cparser::NK_MINUS_EXPR: operation = FloatingOperation::subtract; break;
    case cparser::NK_MULT_EXPR: operation = FloatingOperation::multiply; break;
    case cparser::NK_TRUNC_DIV_EXPR:
        if (!true_value(*rhs)) {
            constant_error(node, context.source, context.diagnostics,
                           "division by zero in floating-point constant expression");
            return std::nullopt;
        }
        operation = FloatingOperation::divide;
        break;
    default:
        return std::nullopt;
    }
    const auto result = floating_arithmetic(operation, a, b);
    if (result)
        return Constant{common, result.bits, result.high_word};
    constant_error(node, context.source, context.diagnostics,
                   "floating-point constant expression is out of range for its type");
    return std::nullopt;
}

std::optional<Constant> evaluate_binary(Node *node, const TypeContext &context,
                                        const ConstantAnalysis &analysis) {
    const auto &source = context.source;
    auto &diagnostics = context.diagnostics;
    const auto operands = binary_operands(node);
    if (!operands)
        return std::nullopt;

    auto left = evaluate_constant(operands->first, context, analysis);
    if (!left)
        return std::nullopt;

    if (node->getKind() == cparser::NK_LOG_AND_EXPR && !true_value(*left))
        return Constant{integer_type(), 0};
    if (node->getKind() == cparser::NK_LOG_OR_EXPR && true_value(*left))
        return Constant{integer_type(), 1};

    auto right = evaluate_constant(operands->second, context, analysis);
    if (!right)
        return std::nullopt;
    if (node->getKind() == cparser::NK_LOG_AND_EXPR ||
        node->getKind() == cparser::NK_LOG_OR_EXPR) {
        return Constant{integer_type(),
                        static_cast<std::uint64_t>(true_value(*right))};
    }

    if (left->type.real() || right->type.real())
        return evaluate_floating_binary(node, *left, *right, context);

    if (node->getKind() == cparser::NK_LSHIFT_EXPR ||
        node->getKind() == cparser::NK_RSHIFT_EXPR) {
        const CType result_type = promote(left->type);
        *left = converted(*left, result_type);
        *right = converted(*right, promote(right->type));
        const auto count = positive_count(*right);
        if (!count || *count >= result_type.bits) {
            constant_error(node, source, diagnostics,
                           "constant shift count is negative or exceeds the operand width");
            return std::nullopt;
        }

        const std::uint64_t raw = normalize(left->value, result_type) &
                                  bit_mask(result_type.bits);
        std::uint64_t result = 0;
        if (node->getKind() == cparser::NK_LSHIFT_EXPR) {
            if (result_type.is_signed) {
                const std::int64_t signed_left =
                    std::bit_cast<std::int64_t>(normalize(left->value, result_type));
                if (signed_left < 0) {
                    constant_error(node, source, diagnostics,
                                   "left shift of a negative signed value is undefined");
                    return std::nullopt;
                }
                const __int128 shifted =
                    static_cast<__int128>(signed_left) << *count;
                const __int128 maximum =
                    result_type.bits == 64
                        ? static_cast<__int128>(
                              std::numeric_limits<std::int64_t>::max())
                        : (static_cast<__int128>(1) << (result_type.bits - 1)) - 1;
                if (shifted > maximum) {
                    constant_error(node, source, diagnostics,
                                   "signed left shift is not representable");
                    return std::nullopt;
                }
                result = static_cast<std::uint64_t>(shifted);
            } else {
                result = raw << *count;
            }
        } else if (!result_type.is_signed ||
                   (raw & (std::uint64_t{1} << (result_type.bits - 1))) == 0) {
            result = raw >> *count;
        } else {
            result = raw >> *count;
            const std::uint64_t fill =
                bit_mask(result_type.bits) ^
                bit_mask(result_type.bits - static_cast<unsigned>(*count));
            result |= fill;
        }
        return Constant{result_type, normalize(result, result_type)};
    }

    const CType common = common_integer(left->type, right->type);
    if (!common.valid())
        return std::nullopt;
    *left = converted(*left, common);
    *right = converted(*right, common);

    const std::uint64_t lhs = normalize(left->value, common);
    const std::uint64_t rhs = normalize(right->value, common);
    const bool comparison = node->getKind() >= cparser::NK_LT_EXPR &&
                            node->getKind() <= cparser::NK_NE_EXPR;

    if (comparison) {
        bool result = false;
        if (node->getKind() == cparser::NK_EQ_EXPR)
            result = lhs == rhs;
        else if (node->getKind() == cparser::NK_NE_EXPR)
            result = lhs != rhs;
        else if (common.is_signed) {
            const auto signed_lhs = std::bit_cast<std::int64_t>(lhs);
            const auto signed_rhs = std::bit_cast<std::int64_t>(rhs);
            if (node->getKind() == cparser::NK_LT_EXPR)
                result = signed_lhs < signed_rhs;
            else if (node->getKind() == cparser::NK_LE_EXPR)
                result = signed_lhs <= signed_rhs;
            else if (node->getKind() == cparser::NK_GT_EXPR)
                result = signed_lhs > signed_rhs;
            else
                result = signed_lhs >= signed_rhs;
        } else {
            if (node->getKind() == cparser::NK_LT_EXPR)
                result = lhs < rhs;
            else if (node->getKind() == cparser::NK_LE_EXPR)
                result = lhs <= rhs;
            else if (node->getKind() == cparser::NK_GT_EXPR)
                result = lhs > rhs;
            else
                result = lhs >= rhs;
        }
        return Constant{integer_type(), static_cast<std::uint64_t>(result)};
    }

    std::uint64_t result = 0;
    if (node->getKind() == cparser::NK_BIT_AND_EXPR ||
        node->getKind() == cparser::NK_BIT_IOR_EXPR ||
        node->getKind() == cparser::NK_BIT_XOR_EXPR) {
        const auto left_bits = TypedInt::point(lhs, common.bits);
        const auto right_bits = TypedInt::point(rhs, common.bits);
        TypedInt value;
        if (node->getKind() == cparser::NK_BIT_AND_EXPR)
            value = left_bits.bit_and(right_bits);
        else if (node->getKind() == cparser::NK_BIT_IOR_EXPR)
            value = left_bits.bit_or(right_bits);
        else
            value = left_bits.bit_xor(right_bits);
        result = value.bits.one;
    } else if (!common.is_signed) {
        switch (node->getKind()) {
        case cparser::NK_PLUS_EXPR:
            result = lhs + rhs;
            break;
        case cparser::NK_MINUS_EXPR:
            result = lhs - rhs;
            break;
        case cparser::NK_MULT_EXPR:
            result = lhs * rhs;
            break;
        case cparser::NK_TRUNC_DIV_EXPR:
        case cparser::NK_TRUNC_MOD_EXPR:
            if (rhs == 0) {
                constant_error(node, source, diagnostics,
                               "division by zero in integer constant expression");
                return std::nullopt;
            }
            result = node->getKind() == cparser::NK_TRUNC_DIV_EXPR ? lhs / rhs
                                                                   : lhs % rhs;
            break;
        default:
            return std::nullopt;
        }
    } else {
        const auto signed_lhs = std::bit_cast<std::int64_t>(lhs);
        const auto signed_rhs = std::bit_cast<std::int64_t>(rhs);
        const __int128 minimum =
            common.bits == 64
                ? static_cast<__int128>(std::numeric_limits<std::int64_t>::min())
                : -(static_cast<__int128>(1) << (common.bits - 1));
        const __int128 maximum =
            common.bits == 64
                ? static_cast<__int128>(std::numeric_limits<std::int64_t>::max())
                : (static_cast<__int128>(1) << (common.bits - 1)) - 1;
        __int128 wide = 0;
        switch (node->getKind()) {
        case cparser::NK_PLUS_EXPR:
            wide = static_cast<__int128>(signed_lhs) + signed_rhs;
            break;
        case cparser::NK_MINUS_EXPR:
            wide = static_cast<__int128>(signed_lhs) - signed_rhs;
            break;
        case cparser::NK_MULT_EXPR:
            wide = static_cast<__int128>(signed_lhs) * signed_rhs;
            break;
        case cparser::NK_TRUNC_DIV_EXPR:
        case cparser::NK_TRUNC_MOD_EXPR:
            if (signed_rhs == 0) {
                constant_error(node, source, diagnostics,
                               "division by zero in integer constant expression");
                return std::nullopt;
            }
            if (static_cast<__int128>(signed_lhs) == minimum && signed_rhs == -1) {
                constant_error(node, source, diagnostics,
                               "signed division overflows in integer constant expression");
                return std::nullopt;
            }
            wide = node->getKind() == cparser::NK_TRUNC_DIV_EXPR
                       ? static_cast<__int128>(signed_lhs / signed_rhs)
                       : static_cast<__int128>(signed_lhs % signed_rhs);
            break;
        default:
            return std::nullopt;
        }
        if (wide < minimum || wide > maximum) {
            constant_error(node, source, diagnostics,
                           "signed overflow in integer constant expression");
            return std::nullopt;
        }
        result = static_cast<std::uint64_t>(wide);
    }
    return Constant{common, normalize(result, common)};
}

std::optional<Constant> evaluate_constant(Node *node, const TypeContext &context,
                                          const ConstantAnalysis &analysis) {
    const AnalysisDepth depth(semantic_depth);
    const auto &source = context.source;
    auto &diagnostics = context.diagnostics;
    if (!present(node))
        return std::nullopt;
    switch (node->getKind()) {
    case cparser::NK_INTEGER_CONST:
        return integer_token_value(node, context);
    case cparser::NK_CHAR_CONST:
        return character_literal(node, source);
    case cparser::NK_REAL_CONST: {
        const auto value = analysis.real_literals.find(node);
        return value == analysis.real_literals.end() ? std::nullopt : std::optional(value->second);
    }
    case cparser::NK_GENERIC_EXPR:
        return evaluate_constant(analysis.selections.at(node), context, analysis);
    case cparser::NK_UNARY_PLUS_EXPR: {
        auto operand = evaluate_constant(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr(),
                                         context, analysis);
        if (operand && operand->type.real())
            return operand;
        return operand ? std::optional<Constant>(converted(*operand, promote(operand->type)))
                       : std::nullopt;
    }
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        if (const auto type = analysis.initializer_types.find(node); type != analysis.initializer_types.end()) {
            auto operand = evaluate_constant(cast->getExpr(), context, analysis);
            return operand ? convert_arithmetic_constant(node, *operand, type->second, context)
                           : std::nullopt;
        }
        CType destination = read_type(cast->getType(), context);
        if (!destination.integer())
            return std::nullopt;
        if (cast->getExpr()->getKind() == cparser::NK_REAL_CONST)
            return cast_real_literal(cast->getExpr(), std::move(destination), source, diagnostics);
        auto operand = evaluate_constant(cast->getExpr(), context, analysis);
        return operand ? std::optional<Constant>(
                             converted(*operand, std::move(destination)))
                       : std::nullopt;
    }
    case cparser::NK_BIT_NOT_EXPR: {
        auto *unary = static_cast<cparser::BitNotExprASTNode *>(node);
        auto operand = evaluate_constant(unary->getExpr(), context, analysis);
        if (!operand || !operand->type.integer())
            return std::nullopt;
        const CType type = promote(operand->type);
        *operand = converted(*operand, type);
        const auto bits = TypedInt::point(operand->value, type.bits).bit_not();
        return Constant{type, normalize(bits.bits.one, type)};
    }
    case cparser::NK_LOG_NOT_EXPR: {
        auto *unary = static_cast<cparser::LogNotExprASTNode *>(node);
        auto operand = evaluate_constant(unary->getExpr(), context, analysis);
        if (!operand || !operand->type.arithmetic())
            return std::nullopt;
        return Constant{integer_type(),
                        static_cast<std::uint64_t>(!true_value(*operand))};
    }
    case cparser::NK_SIZEOF_EXPR:
    case cparser::NK_ALIGNOF_EXPR:
    case cparser::NK_OFFSETOF_EXPR:
        return analysis.sizes.at(node);
    case cparser::NK_MINUS_EXPR: {
        auto *minus = static_cast<cparser::MinusExprASTNode *>(node);
        if (analysis.initializer_types.contains(node)) {
            // 0 - zero is +0, but unary - flips the sign bit of either zero.
            auto operand = evaluate_constant(minus->getRhs(), context, analysis);
            if (!operand)
                return std::nullopt;
            if (operand->type.extended_real())
                operand->high_word ^= operand->type.bits == 128 ? UINT64_C(1) << 63 : 0x8000;
            else
                operand->value ^= UINT64_C(1) << (operand->type.bits - 1);
            return operand;
        }
        return evaluate_binary(node, context, analysis);
    }
    case cparser::NK_LSHIFT_EXPR:
    case cparser::NK_RSHIFT_EXPR:
    case cparser::NK_BIT_IOR_EXPR:
    case cparser::NK_BIT_XOR_EXPR:
    case cparser::NK_BIT_AND_EXPR:
    case cparser::NK_LOG_AND_EXPR:
    case cparser::NK_LOG_OR_EXPR:
    case cparser::NK_PLUS_EXPR:
    case cparser::NK_MULT_EXPR:
    case cparser::NK_TRUNC_DIV_EXPR:
    case cparser::NK_TRUNC_MOD_EXPR:
    case cparser::NK_LT_EXPR:
    case cparser::NK_LE_EXPR:
    case cparser::NK_GT_EXPR:
    case cparser::NK_GE_EXPR:
    case cparser::NK_EQ_EXPR:
    case cparser::NK_NE_EXPR:
        return evaluate_binary(node, context, analysis);
    case cparser::NK_COND_EXPR: {
        auto *conditional = static_cast<cparser::CondExprASTNode *>(node);
        auto condition =
            evaluate_constant(conditional->getCondition(), context, analysis);
        if (!condition)
            return std::nullopt;
        const auto type = analysis.initializer_types.find(node);
        const auto result_type = type == analysis.initializer_types.end()
            ? infer_integer_type(node, context, analysis) : std::optional(type->second);
        if (!result_type)
            return std::nullopt;
        auto *selected = true_value(*condition) ? conditional->getThenClause()
                                                : conditional->getElseClause();
        const auto result = evaluate_constant(selected, context, analysis);
        return result ? convert_arithmetic_constant(node, *result, *result_type, context)
                      : std::nullopt;
    }
    default:
        // Speculative callers use this result to distinguish an ICE from an
        // ordinary expression, so unsupported/non-constant forms are quiet.
        return std::nullopt;
    }
}

CType read_type_list(cparser::SequenceASTNode *sequence,
                      const TypeContext &context,
                      bool allow_incomplete) {
    const auto &source = context.source;
    auto &diagnostics = context.diagnostics;
    const auto nodes = elements(sequence);
    if (nodes.empty()) {
        type_error(sequence, source, diagnostics, "empty type specifier list");
        return {};
    }
    if (nodes.size() == 1)
        return read_type(nodes.front(), context, allow_incomplete);

    bool all_integer = true;
    bool signed_ = true;
    unsigned bits = 0;
    unsigned rank = 0;
    unsigned long_count = 0;
    for (Node *node : nodes) {
        if (!present(node))
            continue;
        if (node->getKind() != cparser::NK_INTEGRAL_TYPE &&
            node->getKind() != cparser::NK_BOOLEAN_TYPE) {
            all_integer = false;
            break;
        }
        CType part = read_type(node, context, allow_incomplete);
        if (!part.integer())
            return {};
        signed_ = signed_ && part.is_signed;
        bits = std::max(bits, part.bits);
        rank = std::max(rank, part.rank);
        if (part.rank == long_rank)
            ++long_count;
    }
    if (!all_integer) {
        type_error(sequence, source, diagnostics,
                   "unsupported combination of type specifiers");
        return {};
    }
    if (long_count >= 2) {
        bits = 64;
        rank = long_long_rank;
    }
    CType result = integer_type(bits, signed_, rank);
    result.is_const = (sequence->getFlags() & TQ_CONST) != 0;
    result.is_volatile = (sequence->getFlags() & TQ_VOLATILE) != 0;
    result.is_restrict = (sequence->getFlags() & TQ_RESTRICT) != 0;
    return result;
}

} // namespace

bool present(Node *node) {
    return node != nullptr && node != cparser::NULL_AST_NODE;
}

std::vector<Node *> elements(Node *node) {
    if (!present(node))
        return {};
    if (node->getKind() != cparser::NK_LIST)
        return {node};
    std::vector<Node *> result;
    for (Node *element :
         static_cast<cparser::SequenceASTNode *>(node)->getElements()) {
        if (present(element))
            result.push_back(element);
    }
    return result;
}

std::string identifier(Node *node) {
    if (!present(node) || node->getKind() != cparser::NK_IDENT_NODE)
        return {};
    return static_cast<cparser::IdentASTNode *>(node)->getValue();
}

Location location(const Preprocessed &source, Node *node) {
    if (!present(node) || node->getLineNum() <= 0)
        return Location{source.file, 0, 0};
    return source.locate(static_cast<unsigned>(node->getLineNum()));
}

CType integer_type(unsigned bits, bool is_signed, unsigned rank) {
    CType result;
    if (bits == 0 || bits > 64)
        return result;
    result.kind = CType::Kind::integer;
    result.bits = bits;
    result.rank = rank == 0 && bits != 1 ? default_rank(bits) : rank;
    result.is_signed = is_signed;
    return result;
}

CType plain_char_type(const Preprocessed &source) {
    auto type = integer_type(8, source.plain_char_signed);
    type.is_plain_char = true;
    return type;
}

CType real_type(unsigned bits) {
    CType result;
    if (bits == 32 || bits == 64 || bits == 80 || bits == 128) {
        result.kind = CType::Kind::real;
        result.bits = bits;
    }
    return result;
}

CType pointer_type(const CType &pointee) {
    CType result;
    if (!pointee.valid())
        return result;
    result.kind = CType::Kind::pointer;
    result.bits = 64;
    result.element = std::make_shared<const CType>(pointee);
    return result;
}

enum class ArrayParameter { no, prototype, definition };

static CType read_array_type(cparser::ArrayTypeASTNode *array, const TypeContext &context,
                             bool allow_incomplete, ArrayParameter parameter) {
    const auto &source = context.source;
    auto &diagnostics = context.diagnostics;
    if (array->hasStarBound() && parameter != ArrayParameter::prototype) {
        type_error(array, source, diagnostics,
                   "array bound '*' is only supported on an outermost array parameter at function prototype scope");
        return {};
    }

    CType element = read_type(array->getElementType(), context);
    if (!element.valid())
        return {};
    if (element.kind == CType::Kind::record && element.record->contains_flexible_array) {
        type_error(array, source, diagnostics,
                   "an array element type cannot contain a flexible array member");
        return {};
    }
    if (element.kind == CType::Kind::void_ || element.kind == CType::Kind::function ||
        (element.kind == CType::Kind::array && !element.count && !element.runtime_array())) {
        type_error(array, source, diagnostics, "array element type must be a complete object type");
        return {};
    }

    std::uint64_t count = 0;
    Node *runtime_bound = nullptr;
    if (Node *bound = array->getExpr(); present(bound)) {
        const auto saved = context.unit.array_bounds.find(bound);
        if (saved != context.unit.array_bounds.end()) {
            if (!saved->second || (*saved->second == 0 && parameter == ArrayParameter::no)) {
                type_error(bound, source, diagnostics, "array bound has invalid type or value");
                return {};
            }
            count = *saved->second;
        } else {
            const auto remember = [&](std::optional<std::uint64_t> value) {
                if (!context.speculative)
                    context.unit.array_bounds.emplace(bound, value);
            };
            const auto before = diagnostics.error_count();
            const auto constant = integer_constant(bound, context);
            const auto value = constant ? positive_count(*constant) : std::nullopt;
            if (value && *value) {
                count = *value;
                remember(value);
            } else {
                if (diagnostics.error_count() != before) {
                    remember(std::nullopt);
                    return {};
                }
                if (constant) {
                    remember(std::nullopt);
                    type_error(bound, source, diagnostics, "array bound must be positive");
                    return {};
                }
                if (parameter == ArrayParameter::no && !context.allow_vla) {
                    remember(std::nullopt);
                    type_error(bound, source, diagnostics, "variable-length arrays are not supported in this declarator");
                    return {};
                }
                const auto type = expression_type(bound, context);
                if (diagnostics.error_count() != before || !type.integer()) {
                    remember(std::nullopt);
                    if (diagnostics.error_count() == before)
                        type_error(bound, source, diagnostics, "array bound must be a valid integer expression");
                    return {};
                }
                if (parameter == ArrayParameter::no) {
                    runtime_bound = bound;
                } else {
                    // Adjustment erases this bound. Only a definition evaluates
                    // the expression on function entry.
                    remember(std::uint64_t{0});
                }
            }
        }
    } else if (!allow_incomplete) {
        type_error(array, source, diagnostics, "incomplete array type is not allowed here");
        return {};
    }

    CType result;
    result.kind = CType::Kind::array;
    result.element = std::make_shared<const CType>(std::move(element));
    result.count = count;
    result.vla_bound = runtime_bound;
    return qualify(std::move(result), array->getFlags() & qualifier_mask);
}

CType read_parameter_type(Node *node, const TypeContext &context, bool definition, Node **entry_bound) {
    if (entry_bound)
        *entry_bound = nullptr;
    if (present(node) && node->getKind() == cparser::NK_ARRAY_TYPE) {
        auto *array = static_cast<cparser::ArrayTypeASTNode *>(node);
        auto type = read_array_type(array, context, true,
                                    definition ? ArrayParameter::definition : ArrayParameter::prototype);
        if (!type.valid())
            return {};
        if (entry_bound && definition && type.count == 0 && present(array->getExpr()))
            *entry_bound = array->getExpr();
        // §6.7.6.3p7: the outer bound does not survive adjustment. A
        // static minimum is a caller precondition, not a dynamic check.
        return qualify(adjust_parameter(std::move(type)), array->getBracketQualifiers());
    }
    auto type = read_type(node, context, true);
    return type.kind == CType::Kind::function ? pointer_type(type) : type;
}

CType read_type(Node *node, const TypeContext &context, bool allow_incomplete,
                bool function_definition) {
    const AnalysisDepth depth(semantic_depth);
    const auto &source = context.source;
    auto &diagnostics = context.diagnostics;
    if (!present(node)) {
        type_error(node, source, diagnostics, "missing type");
        return {};
    }

    const auto qualifiers = node->getFlags() & qualifier_mask;
    if ((qualifiers & TQ_ATOMIC) && !source.native_atomics) {
        type_error(node, source, diagnostics,
                   "_Atomic types require the bundled Linux AMD64 or AArch64 backend");
        return {};
    }
    auto qualify_node = [&, qualifiers](CType type) {
        if ((qualifiers & TQ_ATOMIC) &&
            (!(type.integer() || (type.pointer() && !type.function_pointer())) || type.is_restrict ||
             (qualifiers & TQ_RESTRICT))) {
            type_error(node, source, diagnostics,
                       "_Atomic supports integer and object-pointer types without restrict");
            return CType{};
        }
        return qualify(std::move(type), qualifiers);
    };

    switch (node->getKind()) {
    case cparser::NK_LIST:
        return read_type_list(static_cast<cparser::SequenceASTNode *>(node), context,
                              allow_incomplete);
    case cparser::NK_VOID_TYPE: {
        CType type;
        type.kind = CType::Kind::void_;
        return qualify_node(type);
    }
    case cparser::NK_BOOLEAN_TYPE:
        return qualify_node(integer_type(1, false, bool_rank));
    case cparser::NK_INTEGRAL_TYPE: {
        auto *integer = static_cast<cparser::IntegralTypeASTNode *>(node);
        const unsigned bytes = integer->getAlignment();
        if (bytes == 0 || bytes > 8) {
            type_error(node, source, diagnostics,
                       "unsupported integer width in parser AST");
            return {};
        }
        auto type = integer_type(bytes * 8,
                                 integer->isPlainChar() ? source.plain_char_signed : integer->getIsSigned(),
                                 integer->getRank());
        type.is_plain_char = integer->isPlainChar();
        return qualify_node(type);
    }
    case cparser::NK_ENUMERAL_TYPE: {
        auto *enumeration = static_cast<cparser::EnumeralTypeASTNode *>(node);
        // C11 6.7.2.2p4: choose int as the compatible type. Keep identity
        // for comparisons between enumerated types, not for enumerators.
        auto type = integer_type();
        type.enum_identity = enumeration->getIdentity();
        if (!type.enum_identity)
            type.enum_identity = node;
        TagObserver::notify(context.unit, node, type);
        if (!evaluate_enumerators(enumeration->getBody(), context))
            return {};
        return qualify_node(type);
    }
    case cparser::NK_POINTER_TYPE: {
        auto *pointer = static_cast<cparser::PointerTypeASTNode *>(node);
        auto pointee_context = context;
        pointee_context.allow_vla = false; // VM pointer declarators are not supported.
        CType pointee =
            read_type(pointer->getBaseType(), pointee_context, true);
        CType result = pointer_type(pointee);
        if (!result.valid())
            return {};
        return qualify_node(std::move(result));
    }
    case cparser::NK_ARRAY_TYPE:
        if (qualifiers & TQ_ATOMIC)
            return qualify_node(CType{});
        return read_array_type(static_cast<cparser::ArrayTypeASTNode *>(node), context,
                               allow_incomplete, ArrayParameter::no);
    case cparser::NK_FUNCTION_TYPE: {
        auto *function = static_cast<cparser::FunctionTypeASTNode *>(node);
        CType result_type =
            read_type(function->getType(), context, true);
        if (!result_type.valid())
            return {};
        if (result_type.is_atomic) {
            type_error(node, source, diagnostics, "atomic function return types are not supported");
            return {};
        }
        if (result_type.kind == CType::Kind::array ||
            result_type.kind == CType::Kind::function) {
            type_error(node, source, diagnostics,
                       "function has an unsupported return type");
            return {};
        }

        CType result;
        result.kind = CType::Kind::function;
        result.element =
            std::make_shared<const CType>(unqualified(std::move(result_type)));
        result.prototype = function->hasPrototype();
        result.variadic = function->isVariadic();

        const auto parameters = elements(function->getPrms());
        if (result.variadic && (!result.prototype || parameters.empty())) {
            type_error(node, source, diagnostics,
                       "variadic function must have a fixed parameter");
            return {};
        }
        PrototypeScope prototype{context.prototype, {}};
        auto parameter_context = context;
        parameter_context.prototype = &prototype;
        for (Node *parameter : parameters) {
            Node *type_node = parameter;
            Node *name = nullptr;
            if (parameter->getKind() == cparser::NK_PARM_DECL) {
                auto *declaration = static_cast<cparser::ParmDeclASTNode *>(parameter);
                type_node = declaration->getType();
                name = declaration->getName();
            }
            CType type = read_parameter_type(type_node, parameter_context, function_definition);
            if (!type.valid())
                return {};
            if (type.is_atomic) {
                type_error(parameter, source, diagnostics, "atomic parameters by value are not supported");
                return {};
            }
            if (type.kind == CType::Kind::void_) {
                if (parameters.size() != 1 || result.variadic || present(name) ||
                    type.is_const || type.is_volatile || type.is_restrict) {
                    type_error(parameter, source, diagnostics,
                               "void must be the sole unnamed, unqualified function parameter");
                    return {};
                }
                break;
            }
            if (function_definition && !size_of(type)) {
                type_error(parameter, source, diagnostics,
                           "function definition parameter must have complete type after adjustment");
                return {};
            }
            result.params.push_back(unqualified(type));
            if (present(name))
                prototype.parameters.emplace(identifier(name),
                    TypeBinding{std::move(type), (parameter->getFlags() & SCS_REGISTER) != 0});
        }
        return result;
    }
    case cparser::NK_STRUCT_TYPE:
    case cparser::NK_UNION_TYPE: {
        auto member_context = context;
        member_context.allow_vla = false;
        auto record = read_record_type(node, member_context);
        if (record.valid() && !allow_incomplete && !size_of(record)) {
            type_error(node, source, diagnostics, "incomplete structure or union type");
            return {};
        }
        return qualify_node(std::move(record));
    }
    case cparser::NK_UNKNOWN_TYPE:
        type_error(node, source, diagnostics, "unknown or incomplete parser type");
        return {};
    case cparser::NK_REAL_TYPE: {
        auto *real = static_cast<cparser::RealTypeASTNode *>(node);
        const auto bytes = real->getAlignment();
        if (real->getIsDouble() ? bytes != 8 : bytes != 4) {
            if (real->getIsDouble() && bytes == 16 && source.native_binary128)
                return qualify_node(real_type(128));
            if (real->getIsDouble() && bytes == 16 && source.native_amd64_x87)
                return qualify_node(real_type(80));
            type_error(node, source, diagnostics, "long double floating-point types are not supported");
            return {};
        }
        if (!source.native_fp_types()) {
            type_error(node, source, diagnostics, "floating-point types require a supported native FP ABI");
            return {};
        }
        return qualify_node(real_type(bytes * 8));
    }
    case cparser::NK_COMPLEX_TYPE:
    case cparser::NK_REFERENCE_TYPE:
        type_error(node, source, diagnostics,
                   "complex and reference types are not supported");
        return {};
    default:
        type_error(node, source, diagnostics, "AST node is not a supported C type");
        return {};
    }
}

void bind_typedef_bounds(Node *node, const TypeContext &context) {
    // File-scope typedefs of otherwise unsupported types have historically
    // been allowed if they go unused. Force resolution for array bounds and
    // record/enum definitions. Also resolve arrays of records even without a bound:
    // a record containing a flexible array is already disallowed as an element.
    const auto needs_binding = [](auto &&self, Node *type) -> bool {
        const AnalysisDepth depth(semantic_depth);
        if (!present(type))
            return false;
        switch (type->getKind()) {
        case cparser::NK_ARRAY_TYPE: {
            auto *array = static_cast<cparser::ArrayTypeASTNode *>(type);
            auto *element = array->getElementType();
            return array->hasStarBound() || present(array->getExpr()) ||
                (present(element) && (element->getKind() == cparser::NK_STRUCT_TYPE ||
                                      element->getKind() == cparser::NK_UNION_TYPE)) ||
                self(self, element);
        }
        case cparser::NK_POINTER_TYPE:
            return self(self, static_cast<cparser::PointerTypeASTNode *>(type)->getBaseType());
        case cparser::NK_FUNCTION_TYPE: {
            auto *function = static_cast<cparser::FunctionTypeASTNode *>(type);
            if (self(self, function->getType()))
                return true;
            for (Node *parameter : elements(function->getPrms()))
                if (self(self, parameter))
                    return true;
            return false;
        }
        case cparser::NK_PARM_DECL:
            return self(self, static_cast<cparser::ParmDeclASTNode *>(type)->getType());
        case cparser::NK_STRUCT_TYPE:
            return present(static_cast<cparser::StructTypeASTNode *>(type)->getBody());
        case cparser::NK_UNION_TYPE:
            return present(static_cast<cparser::UnionTypeASTNode *>(type)->getBody());
        case cparser::NK_ENUMERAL_TYPE:
            return present(static_cast<cparser::EnumeralTypeASTNode *>(type)->getBody());
        default:
            return false;
        }
    };
    if (needs_binding(needs_binding, node))
        (void)read_type(node, context, true);
}

CType promote(const CType &type) {
    if (!type.integer())
        return {};
    CType value = unqualified(type);
    value.enum_identity = nullptr;
    const auto width = value.bit_field_width;
    value.bit_field_width = 0;
    value.denotes_bit_field = false;
    if (width && width < 32)
        return integer_type();
    if (value.rank >= int_rank)
        return value;
    if (value.is_signed || value.bits < 32)
        return integer_type();
    return integer_type(32, false, int_rank);
}

CType common_integer(const CType &left, const CType &right) {
    if (!left.integer() || !right.integer())
        return {};
    CType lhs = promote(left);
    CType rhs = promote(right);
    if (lhs.is_signed == rhs.is_signed) {
        if (rhs.rank > lhs.rank ||
            (rhs.rank == lhs.rank && rhs.bits > lhs.bits))
            return rhs;
        return lhs;
    }

    CType unsigned_type = lhs.is_signed ? rhs : lhs;
    CType signed_type = lhs.is_signed ? lhs : rhs;
    if (unsigned_type.rank >= signed_type.rank)
        return unsigned_type;
    if (signed_type.bits > unsigned_type.bits)
        return signed_type;
    return integer_type(signed_type.bits, false, signed_type.rank);
}

CType common_arithmetic(const CType &lhs, const CType &rhs) {
    if (!lhs.arithmetic() || !rhs.arithmetic())
        return {};
    if (lhs.real() || rhs.real())
        return real_type(std::max(lhs.real() ? lhs.bits : 0u, rhs.real() ? rhs.bits : 0u));
    return common_integer(lhs, rhs);
}

bool compatible(const CType &left, const CType &right) {
    if (left.kind != right.kind || left.is_atomic != right.is_atomic || left.is_const != right.is_const ||
        left.is_volatile != right.is_volatile || left.is_restrict != right.is_restrict)
        return false;
    switch (left.kind) {
    case CType::Kind::invalid:
        return false;
    case CType::Kind::void_:
        return true;
    case CType::Kind::integer:
        return (!left.enum_identity || !right.enum_identity ||
                left.enum_identity == right.enum_identity) &&
               left.bits == right.bits && left.rank == right.rank &&
               left.is_signed == right.is_signed && left.is_plain_char == right.is_plain_char;
    case CType::Kind::real:
        return left.bits == right.bits;
    case CType::Kind::pointer:
        return left.element && right.element &&
               compatible(*left.element, *right.element);
    case CType::Kind::array:
        return left.element && right.element &&
               compatible(*left.element, *right.element) &&
               (left.count == 0 || right.count == 0 || left.count == right.count);
    case CType::Kind::record:
        return tagged_record_equal(left, right);
    case CType::Kind::function:
        if (!left.element || !right.element ||
            !compatible(*left.element, *right.element))
            return false;
        if (left.prototype && right.prototype) {
            if (left.variadic != right.variadic ||
                left.params.size() != right.params.size())
                return false;
            for (std::size_t index = 0; index < left.params.size(); ++index) {
                if (!compatible(left.params[index], right.params[index]))
                    return false;
            }
            return true;
        }
        if (!left.prototype && !right.prototype)
            return true;

        const CType &prototype = left.prototype ? left : right;
        if (prototype.variadic)
            return false;
        for (const CType &parameter : prototype.params) {
            CType promoted = parameter.integer() ? promote(parameter) :
                parameter.real() && parameter.bits == 32 ? real_type(64) : parameter;
            if (!compatible(parameter, promoted))
                return false;
        }
        return true;
    }
    return false;
}

CType composite_type(const CType &prior, CType next) {
    if (!next.enum_identity)
        next.enum_identity = prior.enum_identity;
    if (next.kind == CType::Kind::array && !next.count) {
        if (prior.count) {
            next.count = prior.count;
            next.vla_bound = nullptr;
        } else if (!next.vla_bound) {
            next.vla_bound = prior.vla_bound;
            next.vla_size = prior.vla_size;
        }
    }
    if (next.kind == CType::Kind::function && prior.prototype) {
        if (!next.prototype) {
            next.prototype = true;
            next.params = prior.params;
            next.variadic = prior.variadic;
        } else {
            for (std::size_t i = 0; i < next.params.size(); ++i)
                next.params[i] = composite_type(prior.params[i], std::move(next.params[i]));
        }
    }
    // Pointer targets, array elements, and function results are all derived
    // types; information on either side may complete a different subtree.
    if (prior.element && next.element)
        next.element = std::make_shared<const CType>(composite_type(*prior.element, *next.element));
    return next;
}

Node *select_generic(Node *node, const TypeContext &context, CType *result_type) {
    const AnalysisDepth depth(semantic_depth);
    GenericAnalysis local{context, {}};
    struct Restore {
        GenericAnalysis *previous = generic_analysis;
        ~Restore() { generic_analysis = previous; }
    } restore;
    if (!generic_analysis || &generic_analysis->context != &context)
        generic_analysis = &local;
    auto &resolved = generic_analysis->resolved;
    if (auto saved = resolved.find(node); saved != resolved.end()) {
        if (result_type)
            *result_type = saved->second.type;
        return saved->second.selected;
    }

    auto *selection = static_cast<cparser::GenericExprASTNode *>(node);
    const auto &source = context.source;
    auto &diagnostics = context.diagnostics;

    const auto before_control = diagnostics.error_count();
    auto control = expression_type(selection->getControl(), context);
    if (!control.valid()) {
        if (diagnostics.error_count() == before_control)
            type_error(selection->getControl(), source, diagnostics,
                       "invalid controlling expression in _Generic");
        return nullptr;
    }

    // Apply the value conversions in the type domain only: in particular,
    // do not promote integers, load a volatile lvalue, or evaluate a call.
    control = adjust_parameter(std::move(control));
    const auto associations = elements(selection->getAssociations());
    constexpr std::size_t association_limit = 1024;
    if (associations.size() > association_limit) {
        type_error(node, source, diagnostics, "too many _Generic associations");
        return nullptr;
    }
    std::vector<CType> seen_types;
    Node *matched = nullptr;
    Node *fallback = nullptr;
    CType matched_type, fallback_type;
    for (Node *arm : associations) {
        auto *association = static_cast<cparser::GenericAssocASTNode *>(arm);
        const bool is_default = !present(association->getType());
        bool matches = false;
        if (is_default) {
            if (fallback) {
                type_error(arm, source, diagnostics, "duplicate default _Generic association");
                return nullptr;
            }
        } else {
            const auto before_type = diagnostics.error_count();
            auto type = read_type(association->getType(), context, true);
            if (!type.valid() || !size_of(type)) {
                if (diagnostics.error_count() == before_type)
                    type_error(arm, source, diagnostics,
                               "_Generic association must name a complete object type");
                return nullptr;
            }
            if (std::any_of(seen_types.begin(), seen_types.end(), [&](const CType &seen) {
                    return compatible(seen, type);
                })) {
                type_error(arm, source, diagnostics,
                           "compatible types in multiple _Generic associations");
                return nullptr;
            }
            matches = compatible(control, type);
            if (matches && matched) {
                type_error(arm, source, diagnostics,
                           "_Generic controlling type matches multiple associations");
                return nullptr;
            }
            seen_types.push_back(std::move(type));
        }

        auto *expr = association->getExpr();
        const auto before_expr = diagnostics.error_count();
        CType type;
        Node *result = expr;
        if (expr->getKind() == cparser::NK_GENERIC_EXPR)
            result = select_generic(expr, context, &type);
        else
            type = expression_type(expr, context);
        if (!type.valid()) {
            if (diagnostics.error_count() == before_expr)
                type_error(expr, source, diagnostics,
                           "invalid expression in _Generic association");
            return nullptr;
        }
        if (matches) {
            matched = result;
            matched_type = std::move(type);
        } else if (is_default) {
            fallback = result;
            fallback_type = std::move(type);
        }
    }
    if (!matched && !fallback) {
        type_error(node, source, diagnostics,
                   "_Generic controlling type matches no association and there is no default");
        return nullptr;
    }
    auto *selected = matched ? matched : fallback;
    auto type = matched ? std::move(matched_type) : std::move(fallback_type);
    if (result_type)
        *result_type = type;
    resolved.emplace(node, GenericResolution{selected, std::move(type)});
    return selected;
}

std::optional<std::uint64_t> size_of(const CType &type) {
    switch (type.kind) {
    case CType::Kind::real:
        return type.extended_real() ? 16 : type.bits / 8;
    case CType::Kind::integer:
        if (type.bits == 0 || type.bits > 64)
            return std::nullopt;
        return (type.bits + 7) / 8;
    case CType::Kind::pointer:
        return 8;
    case CType::Kind::array: {
        if (!type.element || type.count == 0)
            return std::nullopt;
        const auto element_size = size_of(*type.element);
        if (!element_size ||
            type.count > std::numeric_limits<std::uint64_t>::max() / *element_size)
            return std::nullopt;
        return type.count * *element_size;
    }
    case CType::Kind::record:
        return type.record && type.record->size != 0
            ? std::optional<std::uint64_t>(type.record->size) : std::nullopt;
    case CType::Kind::invalid:
    case CType::Kind::void_:
    case CType::Kind::function:
        return std::nullopt;
    }
    return std::nullopt;
}

unsigned align_of(const CType &type) {
    if (type.extended_real())
        return 16;
    switch (type.kind) {
    case CType::Kind::real:
    case CType::Kind::integer: {
        const auto size = size_of(type);
        return size ? static_cast<unsigned>(std::min<std::uint64_t>(*size, 8)) : 0;
    }
    case CType::Kind::pointer:
        return 8;
    case CType::Kind::array:
        return type.element ? align_of(*type.element) : 0;
    case CType::Kind::record:
        return type.record ? type.record->alignment : 0;
    case CType::Kind::invalid:
    case CType::Kind::void_:
    case CType::Kind::function:
        return 0;
    }
    return 0;
}

std::string_view mir_type(const CType &type) {
    switch (type.kind) {
    case CType::Kind::void_:
        return "void";
    case CType::Kind::integer:
        return "i64";
    case CType::Kind::real:
        return type.bits == 32 ? "f32" : type.bits == 64 ? "f64" : type.bits == 128 ? "f128" : type.extended_real() ? "f80" : "";
    case CType::Kind::pointer:
        return type.function_pointer() ? "func_ptr" : "ptr";
    case CType::Kind::invalid:
    case CType::Kind::function:
    case CType::Kind::array:
    case CType::Kind::record:
        return {};
    }
    return {};
}

std::uint64_t normalize(std::uint64_t value, const CType &type) {
    if (!type.integer() || type.bits == 0 || type.bits > 64)
        return value;
    if (type.bits == 1 && type.rank == bool_rank)
        return value != 0;

    const auto normalized = TypedInt::point(value, type.bits).bits.one;
    if (!type.is_signed || type.bits == 64)
        return normalized;
    const std::uint64_t sign = std::uint64_t{1} << (type.bits - 1);
    return (normalized & sign) != 0 ? normalized | ~bit_mask(type.bits)
                                    : normalized;
}

static std::optional<Constant> member_offset(Node *node, const TypeContext &context) {
    auto *offset = static_cast<cparser::OffsetOfExprASTNode *>(node);
    auto type = read_type(offset->getType(), context, true);
    const auto error = [&](Node *at, const char *message) -> std::optional<Constant> {
        type_error(at, context.source, context.diagnostics, message);
        return std::nullopt;
    };
    if (!type.valid())
        return std::nullopt;
    if (type.kind != CType::Kind::record || !size_of(type))
        return error(node, "offsetof requires a complete struct or union type");

    std::uint64_t bytes = 0;
    for (Node *part : elements(offset->getDesignators())) {
        if (part->getKind() == cparser::NK_IDENT_NODE) {
            const auto member = find_member(type, identifier(part));
            if (!member)
                return error(part, "offsetof designator names no such record member");
            if (member->bit_field)
                return error(part, "offsetof cannot name a bit-field");
            bytes += member->offset;
            type = member->type;
        } else {
            if (type.kind != CType::Kind::array || !type.element || !type.count)
                return error(part, "offsetof subscript requires an array of known bound");
            const auto errors = context.diagnostics.error_count();
            auto *expr = static_cast<cparser::ArrayDesignatorASTNode *>(part)->getIndex();
            const auto index = integer_constant(expr, context);
            if (!index || !index->type.integer()) {
                if (context.diagnostics.error_count() != errors)
                    return std::nullopt;
                return error(part, "offsetof subscript requires an integer constant expression");
            }
            const auto i = normalize(index->value, index->type);
            const auto size = size_of(*type.element);
            if (i >= type.count || !size)
                return error(part, "offsetof subscript is outside the array bound");
            // The layout of the complete containing type bounds the sum.
            bytes += i * *size;
            const auto element = type.element; // Keep it alive while copying the type.
            type = *element;
        }
    }
    return Constant{integer_type(64, false, long_rank), bytes};
}

CType layout_operand_type(Node *node, const TypeContext &context) {
    const AnalysisDepth depth(semantic_depth);
    const bool alignment = node->getKind() == cparser::NK_ALIGNOF_EXPR;
    Node *operand = alignment ? static_cast<cparser::AlignOfExprASTNode *>(node)->getExpr()
                              : static_cast<cparser::SizeOfExprASTNode *>(node)->getExpr();
    const auto errors = context.diagnostics.error_count();
    CType type;
    if (present(operand) && operand->isType()) {
        auto operand_context = context;
        operand_context.allow_vla = context.function != nullptr;
        type = read_type(operand, operand_context, true);
    } else if (!alignment) {
        type = expression_type(operand, context);
    }
    if (type.denotes_bit_field) {
        constant_error(node, context.source, context.diagnostics, "sizeof cannot be applied to a bit-field");
        return {};
    }
    if ((type.runtime_array() || size_of(type)) && (!alignment || align_of(type)))
        return type;
    if (context.diagnostics.error_count() == errors)
        constant_error(node, context.source, context.diagnostics,
                       alignment ? "_Alignof requires a complete object type"
                                 : "sizeof requires a complete object type and a valid operand");
    return {};
}

std::optional<Constant> layout_constant(Node *node, const TypeContext &context) {
    if (node->getKind() == cparser::NK_OFFSETOF_EXPR)
        return member_offset(node, context);
    const auto type = layout_operand_type(node, context);
    if (type.valid()) {
        if (node->getKind() == cparser::NK_ALIGNOF_EXPR)
            return Constant{integer_type(64, false, long_rank), align_of(type)};
        if (const auto size = size_of(type))
            return Constant{integer_type(64, false, long_rank), *size};
    }
    return std::nullopt;
}

std::optional<Constant> integer_constant(Node *node, const TypeContext &context) {
    ConstantAnalysis analysis;
    return has_ice_form(node, context, analysis) ? evaluate_constant(node, context, analysis)
                                                 : std::nullopt;
}

std::optional<Constant> arithmetic_initializer_constant(Node *node, const CType &destination,
                                                        const TypeContext &context) {
    ConstantAnalysis analysis;
    if (!analyze_arithmetic_initializer(node, context, analysis).arithmetic())
        return std::nullopt;
    const auto value = evaluate_constant(node, context, analysis);
    return value ? convert_arithmetic_constant(node, *value, destination, context) : std::nullopt;
}

std::optional<unsigned> alignment_specifiers(Node *specifiers, const TypeContext &context) {
    if (!present(specifiers))
        return 0;
    if (auto found = context.unit.alignment_values.find(specifiers);
        found != context.unit.alignment_values.end())
        return found->second;

    const auto evaluate = [&]() -> std::optional<unsigned> {
        unsigned strictest = 0;
        for (Node *operand : elements(specifiers)) {
            const auto errors = context.diagnostics.error_count();
            const auto value = integer_constant(operand, context);
            if (!value || !value->type.integer()) {
                if (context.diagnostics.error_count() == errors)
                    type_error(operand, context.source, context.diagnostics,
                               "_Alignas requires an integer constant expression");
                return {};
            }
            const auto bytes = normalize(value->value, value->type);
            if (bytes > max_object_alignment || (bytes != 0 && (bytes & (bytes - 1)) != 0)) {
                type_error(operand, context.source, context.diagnostics,
                           "_Alignas requires a supported alignment (0, 1, 2, 4, 8, or 16)");
                return {};
            }
            strictest = std::max(strictest, static_cast<unsigned>(bytes));
        }
        return strictest;
    };
    const auto value = evaluate();
    if (!context.speculative)
        context.unit.alignment_values.emplace(specifiers, value);
    return value;
}

bool check_object_alignment(unsigned requested, unsigned natural, Node *node,
                            const TypeContext &context) {
    if (requested == 0 || requested >= natural)
        return true;
    type_error(node, context.source, context.diagnostics,
               "_Alignas cannot weaken the natural alignment");
    return false;
}

std::optional<Constant> real_constant(Node *node, const TypeContext &context, bool require_native) {
    if (!present(node) || node->getKind() != cparser::NK_REAL_CONST)
        return std::nullopt;
    if (require_native && !context.source.native_fp_types()) {
        type_error(node, context.source, context.diagnostics,
                   "floating-point expressions require a supported native FP ABI");
        return std::nullopt;
    }
    const auto literal = decode_real_literal(node, context.source, context.diagnostics);
    return literal ? std::optional<Constant>(Constant{real_type(literal.width), literal.bits,
                                                       literal.high_word}) :
                     std::nullopt;
}

CType string_constant_type(Node *node, const Preprocessed &source) {
    const auto *literal = static_cast<cparser::StringConstASTNode *>(node);
    using cparser::StringEncoding;
    CType element;
    switch (literal->getEncoding()) {
    case StringEncoding::ordinary:
    case StringEncoding::utf8:
        element = plain_char_type(source);
        break;
    case StringEncoding::wide: element = integer_type(32, source.wchar_signed); break;
    case StringEncoding::utf16: element = integer_type(16, false); break;
    case StringEncoding::utf32: element = integer_type(32, false); break;
    }
    CType result;
    result.kind = CType::Kind::array;
    result.element = std::make_shared<CType>(element);
    result.count = 1 + (element.is_plain_char ? literal->getBytes().size() :
                                                literal->getWideUnits().size());
    return result;
}

std::string string_constant_bytes(Node *node) {
    const auto *literal = static_cast<cparser::StringConstASTNode *>(node);
    const auto encoding = literal->getEncoding();
    if (!cparser::wideStringEncoding(encoding))
        return literal->getBytes() + '\0';

    // All bundled targets are little-endian. Preserve the bit pattern even
    // for numeric escapes above the signed maximum of wchar_t.
    const auto width = encoding == cparser::StringEncoding::utf16 ? 2u : 4u;
    std::string result;
    result.reserve((literal->getWideUnits().size() + 1) * width);
    for (const auto unit : literal->getWideUnits())
        for (unsigned byte = 0; byte < width; ++byte)
            result.push_back(static_cast<char>((unit >> (8 * byte)) & 0xffu));
    result.append(width, '\0');
    return result;
}

} // namespace cephyr::detail
