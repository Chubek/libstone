#include <cephyr/lower.hpp>

#include <cephyr/preprocess.hpp>

#include <AbstractSyntaxTree.h>
#include <ASTNode.h>
#include <wetstone/abstract-interp/TypedInt.hpp>

#include <algorithm>
#include <bit>
#include <cctype>
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

constexpr unsigned qualifier_mask = TQ_CONST | TQ_VOLATILE;

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
    type.is_const = false;
    type.is_volatile = false;
    return type;
}

CType adjust_parameter(CType type) {
    if (type.kind == CType::Kind::array && type.element) {
        // C11 applies qualifiers on an array typedef to its element type.
        CType element = *type.element;
        element.is_const |= type.is_const;
        element.is_volatile |= type.is_volatile;
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

std::optional<Constant> parse_integer_literal(Node *, const Preprocessed &,
                                              Diagnostics &);
std::optional<CType> infer_integer_type(Node *, const Preprocessed &, Diagnostics &);
std::optional<Constant> evaluate_ice(Node *, const Preprocessed &, Diagnostics &);

thread_local unsigned semantic_depth = 0;

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

bool identifier_spelling(std::string_view text) {
    if (text.empty())
        return false;
    const auto first = static_cast<unsigned char>(text.front());
    if (text.front() != '_' && !std::isalpha(first))
        return false;
    return std::all_of(text.begin() + 1, text.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return character == '_' || std::isalnum(value);
    });
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

std::optional<Constant> parse_integer_literal(Node *node,
                                              const Preprocessed &source,
                                              Diagnostics &diagnostics) {
    if (!present(node) || node->getKind() != cparser::NK_INTEGER_CONST)
        return std::nullopt;
    auto *literal = static_cast<cparser::IntegerConstASTNode *>(node);
    const auto original = spelling(literal);
    if (!original.empty()) {
        // The parser uses identifier spelling for enumerators, but its value
        // is not reliable: it truncates initializers and numbers implicit
        // enumerators independently of the preceding explicit value.
        if (identifier_spelling(original)) {
            constant_error(node, source, diagnostics,
                           "enumerator values are not supported by the C parser");
            return std::nullopt;
        }
        return parse_integer_spelling(node, original, source, diagnostics);
    }
    CType type = integer_type();
    const auto value = literal->getValue();
    if (normalize(value, type) != value) {
        constant_error(node, source, diagnostics,
                       "integer constant without token spelling does not fit in int");
        return std::nullopt;
    }
    return Constant{type, value};
}

bool append_utf8(std::string &output, std::uint32_t value) {
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return false;
    if (value <= 0x7f) {
        output.push_back(static_cast<char>(value));
    } else if (value <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (value >> 6)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (value >> 12)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        output.push_back(static_cast<char>(0xf0 | (value >> 18)));
        output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
    return true;
}

bool decode_escape(std::string_view input, std::size_t &index, std::string &output,
                   std::string &error) {
    if (index >= input.size()) {
        error = "unterminated escape sequence";
        return false;
    }
    const char escaped = input[index++];
    switch (escaped) {
    case '\'':
    case '"':
    case '?':
    case '\\':
        output.push_back(escaped);
        return true;
    case 'a':
        output.push_back('\a');
        return true;
    case 'b':
        output.push_back('\b');
        return true;
    case 'f':
        output.push_back('\f');
        return true;
    case 'n':
        output.push_back('\n');
        return true;
    case 'r':
        output.push_back('\r');
        return true;
    case 't':
        output.push_back('\t');
        return true;
    case 'v':
        output.push_back('\v');
        return true;
    default:
        break;
    }

    if (escaped >= '0' && escaped <= '7') {
        unsigned value = static_cast<unsigned>(escaped - '0');
        unsigned count = 1;
        while (count < 3 && index < input.size() && input[index] >= '0' &&
               input[index] <= '7') {
            value = value * 8 + static_cast<unsigned>(input[index++] - '0');
            ++count;
        }
        if (value > 255) {
            error = "octal escape does not fit in an ordinary character";
            return false;
        }
        output.push_back(static_cast<char>(value));
        return true;
    }

    if (escaped == 'x') {
        std::uint32_t value = 0;
        bool any = false;
        while (index < input.size()) {
            unsigned digit = 0;
            if (!digit_value(input[index], digit))
                break;
            any = true;
            if (value > (255u - digit) / 16u) {
                error = "hexadecimal escape does not fit in an ordinary character";
                return false;
            }
            value = value * 16u + digit;
            ++index;
        }
        if (!any) {
            error = "hexadecimal escape has no digits";
            return false;
        }
        output.push_back(static_cast<char>(value));
        return true;
    }

    if (escaped == 'u' || escaped == 'U') {
        const unsigned digits = escaped == 'u' ? 4 : 8;
        std::uint32_t value = 0;
        for (unsigned count = 0; count < digits; ++count) {
            if (index >= input.size()) {
                error = "incomplete universal character name";
                return false;
            }
            unsigned digit = 0;
            if (!digit_value(input[index++], digit)) {
                error = "invalid universal character name";
                return false;
            }
            value = value * 16u + digit;
        }
        if (!append_utf8(output, value)) {
            error = "unrepresentable universal character name";
            return false;
        }
        return true;
    }

    error = "unknown escape sequence";
    return false;
}

bool decode_characters(std::string_view input, std::string &output,
                       std::string &error) {
    for (std::size_t index = 0; index < input.size();) {
        const char character = input[index++];
        if (character != '\\') {
            output.push_back(character);
            continue;
        }
        if (!decode_escape(input, index, output, error))
            return false;
    }
    return true;
}

bool decode_quoted_strings(std::string_view spelling_text, std::string &output,
                           std::string &error) {
    std::size_t index = 0;
    bool found = false;
    while (index < spelling_text.size()) {
        while (index < spelling_text.size() &&
               std::isspace(static_cast<unsigned char>(spelling_text[index])))
            ++index;
        if (index == spelling_text.size())
            break;

        if (spelling_text.substr(index, 2) == "u8")
            index += 2;
        else if (spelling_text[index] == 'L' || spelling_text[index] == 'u' ||
                 spelling_text[index] == 'U') {
            error = "wide string literals are not supported";
            return false;
        }
        if (index >= spelling_text.size() || spelling_text[index] != '"') {
            error = "malformed string literal spelling";
            return false;
        }
        found = true;
        ++index;
        std::string piece;
        bool closed = false;
        while (index < spelling_text.size()) {
            const char character = spelling_text[index++];
            if (character == '"') {
                closed = true;
                break;
            }
            if (character == '\\') {
                if (!decode_escape(spelling_text, index, piece, error))
                    return false;
            } else {
                piece.push_back(character);
            }
        }
        if (!closed) {
            error = "unterminated string literal";
            return false;
        }
        output += piece;
    }
    if (!found) {
        error = "malformed string literal spelling";
        return false;
    }
    return true;
}

std::optional<Constant> parse_character_literal(Node *node,
                                                const Preprocessed &source,
                                                Diagnostics &diagnostics) {
    if (!present(node) || node->getKind() != cparser::NK_CHAR_CONST)
        return std::nullopt;
    auto *literal = static_cast<cparser::CharConstASTNode *>(node);
    std::uint64_t value = 0;
    if (const auto text = spelling(literal); !text.empty()) {
        if (!text.empty() &&
            (text.front() == 'L' || text.front() == 'u' || text.front() == 'U')) {
            constant_error(node, source, diagnostics,
                           "wide character constants are not supported");
            return std::nullopt;
        }
        if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
            constant_error(node, source, diagnostics,
                           "malformed character constant spelling");
            return std::nullopt;
        }
        std::string decoded;
        std::string error;
        if (!decode_characters(text.substr(1, text.size() - 2), decoded, error)) {
            constant_error(node, source, diagnostics, error);
            return std::nullopt;
        }
        if (decoded.size() != 1) {
            constant_error(node, source, diagnostics,
                           "multi-character constants are not supported");
            return std::nullopt;
        }
        value = static_cast<unsigned char>(decoded.front());
        if (source.plain_char_signed && value >= 128)
            value -= 256; // Sign extension in unsigned arithmetic.
    } else {
        value = static_cast<std::uint64_t>(literal->getValue());
    }
    CType type = integer_type();
    return Constant{type, normalize(value, type)};
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
bool has_ice_form(Node *node) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node))
        return false;
    switch (node->getKind()) {
    case cparser::NK_INTEGER_CONST:
    case cparser::NK_CHAR_CONST:
    case cparser::NK_SIZEOF_EXPR:
    case cparser::NK_ALIGNOF_EXPR:
        return true;
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        auto *type = cast->getType();
        if (!present(type))
            return false;
        return (type->getKind() == cparser::NK_BOOLEAN_TYPE ||
                type->getKind() == cparser::NK_INTEGRAL_TYPE ||
                type->getKind() == cparser::NK_ENUMERAL_TYPE ||
                type->getKind() == cparser::NK_LIST) &&
               has_ice_form(cast->getExpr());
    }
    case cparser::NK_UNARY_PLUS_EXPR:
        return has_ice_form(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr());
    case cparser::NK_BIT_NOT_EXPR:
        return has_ice_form(static_cast<cparser::BitNotExprASTNode *>(node)->getExpr());
    case cparser::NK_LOG_NOT_EXPR:
        return has_ice_form(static_cast<cparser::LogNotExprASTNode *>(node)->getExpr());
    case cparser::NK_COND_EXPR: {
        auto *cond = static_cast<cparser::CondExprASTNode *>(node);
        return has_ice_form(cond->getCondition()) &&
               has_ice_form(cond->getThenClause()) &&
               has_ice_form(cond->getElseClause());
    }
    default:
        const auto operands = binary_operands(node);
        return operands && has_ice_form(operands->first) && has_ice_form(operands->second);
    }
}

std::optional<CType> infer_integer_type(Node *node, const Preprocessed &source,
                                        Diagnostics &diagnostics) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node))
        return std::nullopt;
    switch (node->getKind()) {
    case cparser::NK_INTEGER_CONST: {
        const auto literal = parse_integer_literal(node, source, diagnostics);
        return literal ? std::optional<CType>(literal->type) : std::nullopt;
    }
    case cparser::NK_CHAR_CONST:
        return integer_type();
    case cparser::NK_UNARY_PLUS_EXPR: {
        auto operand = infer_integer_type(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr(),
                                          source, diagnostics);
        return operand ? std::optional<CType>(promote(*operand)) : std::nullopt;
    }
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        CType type = read_type(cast->getType(), source, diagnostics);
        return type.integer() ? std::optional<CType>(unqualified(type))
                              : std::nullopt;
    }
    case cparser::NK_BIT_NOT_EXPR: {
        auto *unary = static_cast<cparser::BitNotExprASTNode *>(node);
        const auto operand = infer_integer_type(unary->getExpr(), source, diagnostics);
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
                ? infer_integer_type(operands->first, source, diagnostics)
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
            infer_integer_type(operands->first, source, diagnostics);
        const auto right =
            infer_integer_type(operands->second, source, diagnostics);
        if (!left || !right)
            return std::nullopt;
        return common_integer(*left, *right);
    }
    case cparser::NK_COND_EXPR: {
        auto *conditional = static_cast<cparser::CondExprASTNode *>(node);
        const auto yes =
            infer_integer_type(conditional->getThenClause(), source, diagnostics);
        const auto no =
            infer_integer_type(conditional->getElseClause(), source, diagnostics);
        if (!yes || !no)
            return std::nullopt;
        return common_integer(*yes, *no);
    }
    case cparser::NK_SIZEOF_EXPR:
    case cparser::NK_ALIGNOF_EXPR:
        return integer_type(64, false, long_rank);
    default:
        return std::nullopt;
    }
}

bool true_value(const Constant &constant) {
    return normalize(constant.value, constant.type) != 0;
}

std::optional<Constant> evaluate_size_or_align(Node *node,
                                              const Preprocessed &source,
                                              Diagnostics &diagnostics,
                                              bool alignment) {
    Node *operand = nullptr;
    if (alignment) {
        operand = static_cast<cparser::AlignOfExprASTNode *>(node)->getExpr();
    } else {
        operand = static_cast<cparser::SizeOfExprASTNode *>(node)->getExpr();
    }

    CType type;
    if (present(operand) && operand->isType()) {
        type = read_type(operand, source, diagnostics);
    } else if (present(operand) && operand->getKind() == cparser::NK_STRING_CONST) {
        const auto bytes = string_constant(operand, source, diagnostics);
        if (!bytes)
            return std::nullopt;
        type.kind = CType::Kind::array;
        type.element = std::make_shared<const CType>(plain_char_type(source));
        type.count = bytes->size();
    } else if (present(operand) && operand->getKind() == cparser::NK_CAST_EXPR) {
        auto *cast = static_cast<cparser::CastExprASTNode *>(operand);
        type = read_type(cast->getType(), source, diagnostics);
    } else {
        const auto inferred = infer_integer_type(operand, source, diagnostics);
        if (!inferred)
            return std::nullopt;
        type = *inferred;
    }

    std::optional<std::uint64_t> value;
    if (alignment) {
        const unsigned result = align_of(type);
        if (result != 0)
            value = result;
    } else {
        value = size_of(type);
    }
    if (!value) {
        constant_error(node, source, diagnostics,
                       alignment ? "_Alignof operand has no supported alignment"
                                 : "sizeof operand has incomplete or unsupported type");
        return std::nullopt;
    }
    CType size_type = integer_type(64, false, long_rank);
    return Constant{size_type, normalize(*value, size_type)};
}

std::optional<Constant> evaluate_binary(Node *node, const Preprocessed &source,
                                       Diagnostics &diagnostics) {
    const auto operands = binary_operands(node);
    if (!operands)
        return std::nullopt;

    auto left = evaluate_ice(operands->first, source, diagnostics);
    if (!left)
        return std::nullopt;

    if (node->getKind() == cparser::NK_LOG_AND_EXPR && !true_value(*left))
        return Constant{integer_type(), 0};
    if (node->getKind() == cparser::NK_LOG_OR_EXPR && true_value(*left))
        return Constant{integer_type(), 1};

    auto right = evaluate_ice(operands->second, source, diagnostics);
    if (!right)
        return std::nullopt;
    if (!left->type.integer() || !right->type.integer())
        return std::nullopt;

    if (node->getKind() == cparser::NK_LOG_AND_EXPR ||
        node->getKind() == cparser::NK_LOG_OR_EXPR) {
        return Constant{integer_type(),
                        static_cast<std::uint64_t>(true_value(*right))};
    }

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

std::optional<Constant> evaluate_ice(Node *node, const Preprocessed &source,
                                    Diagnostics &diagnostics) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node))
        return std::nullopt;
    switch (node->getKind()) {
    case cparser::NK_INTEGER_CONST:
        return parse_integer_literal(node, source, diagnostics);
    case cparser::NK_CHAR_CONST:
        return parse_character_literal(node, source, diagnostics);
    case cparser::NK_UNARY_PLUS_EXPR: {
        auto operand = evaluate_ice(static_cast<cparser::UnaryPlusASTNode *>(node)->getExpr(),
                                    source, diagnostics);
        return operand ? std::optional<Constant>(converted(*operand, promote(operand->type)))
                       : std::nullopt;
    }
    case cparser::NK_CAST_EXPR: {
        auto *cast = static_cast<cparser::CastExprASTNode *>(node);
        CType destination = read_type(cast->getType(), source, diagnostics);
        if (!destination.integer())
            return std::nullopt;
        auto operand = evaluate_ice(cast->getExpr(), source, diagnostics);
        return operand ? std::optional<Constant>(
                             converted(*operand, std::move(destination)))
                       : std::nullopt;
    }
    case cparser::NK_BIT_NOT_EXPR: {
        auto *unary = static_cast<cparser::BitNotExprASTNode *>(node);
        auto operand = evaluate_ice(unary->getExpr(), source, diagnostics);
        if (!operand || !operand->type.integer())
            return std::nullopt;
        const CType type = promote(operand->type);
        *operand = converted(*operand, type);
        const auto bits = TypedInt::point(operand->value, type.bits).bit_not();
        return Constant{type, normalize(bits.bits.one, type)};
    }
    case cparser::NK_LOG_NOT_EXPR: {
        auto *unary = static_cast<cparser::LogNotExprASTNode *>(node);
        auto operand = evaluate_ice(unary->getExpr(), source, diagnostics);
        if (!operand || !operand->type.integer())
            return std::nullopt;
        return Constant{integer_type(),
                        static_cast<std::uint64_t>(!true_value(*operand))};
    }
    case cparser::NK_SIZEOF_EXPR: {
        return evaluate_size_or_align(node, source, diagnostics, false);
    }
    case cparser::NK_ALIGNOF_EXPR:
        return evaluate_size_or_align(node, source, diagnostics, true);
    case cparser::NK_LSHIFT_EXPR:
    case cparser::NK_RSHIFT_EXPR:
    case cparser::NK_BIT_IOR_EXPR:
    case cparser::NK_BIT_XOR_EXPR:
    case cparser::NK_BIT_AND_EXPR:
    case cparser::NK_LOG_AND_EXPR:
    case cparser::NK_LOG_OR_EXPR:
    case cparser::NK_PLUS_EXPR:
    case cparser::NK_MINUS_EXPR:
    case cparser::NK_MULT_EXPR:
    case cparser::NK_TRUNC_DIV_EXPR:
    case cparser::NK_TRUNC_MOD_EXPR:
    case cparser::NK_LT_EXPR:
    case cparser::NK_LE_EXPR:
    case cparser::NK_GT_EXPR:
    case cparser::NK_GE_EXPR:
    case cparser::NK_EQ_EXPR:
    case cparser::NK_NE_EXPR:
        return evaluate_binary(node, source, diagnostics);
    case cparser::NK_COND_EXPR: {
        auto *conditional = static_cast<cparser::CondExprASTNode *>(node);
        auto condition =
            evaluate_ice(conditional->getCondition(), source, diagnostics);
        if (!condition)
            return std::nullopt;
        const auto result_type =
            infer_integer_type(node, source, diagnostics);
        if (!result_type)
            return std::nullopt;
        auto result =
            evaluate_ice(true_value(*condition) ? conditional->getThenClause()
                                                : conditional->getElseClause(),
                         source, diagnostics);
        return result ? std::optional<Constant>(
                            converted(*result, *result_type))
                      : std::nullopt;
    }
    default:
        // Speculative callers use this result to distinguish an ICE from an
        // ordinary expression, so unsupported/non-constant forms are quiet.
        return std::nullopt;
    }
}

CType read_type_list(cparser::SequenceASTNode *sequence,
                     const Preprocessed &source, Diagnostics &diagnostics,
                     bool allow_incomplete) {
    const auto nodes = elements(sequence);
    if (nodes.empty()) {
        type_error(sequence, source, diagnostics, "empty type specifier list");
        return {};
    }
    if (nodes.size() == 1)
        return read_type(nodes.front(), source, diagnostics, allow_incomplete);

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
        CType part = read_type(node, source, diagnostics, allow_incomplete);
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

CType pointer_type(const CType &pointee) {
    CType result;
    if (!pointee.valid())
        return result;
    result.kind = CType::Kind::pointer;
    result.bits = 64;
    result.element = std::make_shared<const CType>(pointee);
    return result;
}

CType read_type(Node *node, const Preprocessed &source, Diagnostics &diagnostics,
                bool allow_incomplete) {
    const AnalysisDepth depth(semantic_depth);
    if (!present(node)) {
        type_error(node, source, diagnostics, "missing type");
        return {};
    }

    const auto qualifiers = node->getFlags() & qualifier_mask;
    auto qualify = [qualifiers](CType type) {
        type.is_const = (qualifiers & TQ_CONST) != 0;
        type.is_volatile = (qualifiers & TQ_VOLATILE) != 0;
        return type;
    };

    switch (node->getKind()) {
    case cparser::NK_LIST:
        return read_type_list(static_cast<cparser::SequenceASTNode *>(node), source,
                              diagnostics, allow_incomplete);
    case cparser::NK_VOID_TYPE: {
        CType type;
        type.kind = CType::Kind::void_;
        return qualify(type);
    }
    case cparser::NK_BOOLEAN_TYPE:
        return qualify(integer_type(1, false, bool_rank));
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
        return qualify(type);
    }
    case cparser::NK_ENUMERAL_TYPE:
        return qualify(integer_type());
    case cparser::NK_POINTER_TYPE: {
        auto *pointer = static_cast<cparser::PointerTypeASTNode *>(node);
        CType pointee =
            read_type(pointer->getBaseType(), source, diagnostics, true);
        CType result = pointer_type(pointee);
        if (!result.valid())
            return {};
        return qualify(std::move(result));
    }
    case cparser::NK_ARRAY_TYPE: {
        auto *array = static_cast<cparser::ArrayTypeASTNode *>(node);
        CType element =
            read_type(array->getElementType(), source, diagnostics, false);
        if (!element.valid())
            return {};
        if (element.kind == CType::Kind::void_ ||
            element.kind == CType::Kind::function) {
            type_error(node, source, diagnostics,
                       "array element type must be a complete object type");
            return {};
        }

        std::uint64_t count = 0;
        if (present(array->getExpr())) {
            const auto constant =
                integer_constant(array->getExpr(), source, diagnostics);
            if (!constant) {
                type_error(array->getExpr(), source, diagnostics,
                           "variable-length arrays are not supported");
                return {};
            }
            if (constant->type.is_signed) {
                const auto signed_count =
                    std::bit_cast<std::int64_t>(
                        normalize(constant->value, constant->type));
                if (signed_count <= 0) {
                    type_error(array->getExpr(), source, diagnostics,
                               "array bound must be positive");
                    return {};
                }
                count = static_cast<std::uint64_t>(signed_count);
            } else {
                count = normalize(constant->value, constant->type);
                if (count == 0) {
                    type_error(array->getExpr(), source, diagnostics,
                               "array bound must be positive");
                    return {};
                }
            }
        } else if (!allow_incomplete) {
            type_error(node, source, diagnostics,
                       "incomplete array type is not allowed here");
            return {};
        }

        CType result;
        result.kind = CType::Kind::array;
        result.element = std::make_shared<const CType>(std::move(element));
        result.count = count;
        return qualify(std::move(result));
    }
    case cparser::NK_FUNCTION_TYPE: {
        auto *function = static_cast<cparser::FunctionTypeASTNode *>(node);
        CType result_type =
            read_type(function->getType(), source, diagnostics, true);
        if (!result_type.valid())
            return {};
        if (result_type.kind == CType::Kind::array ||
            result_type.kind == CType::Kind::function ||
            result_type.kind == CType::Kind::record) {
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
        for (Node *parameter : parameters) {
            Node *type_node = parameter;
            Node *name = nullptr;
            if (parameter->getKind() == cparser::NK_PARM_DECL) {
                auto *declaration = static_cast<cparser::ParmDeclASTNode *>(parameter);
                type_node = declaration->getType();
                name = declaration->getName();
            }
            // An incomplete array parameter is valid because adjustment to a
            // pointer occurs before the completed function type is formed.
            CType type = read_type(type_node, source, diagnostics, true);
            if (!type.valid())
                return {};
            if (type.kind == CType::Kind::void_) {
                if (parameters.size() != 1 || result.variadic || present(name) ||
                    type.is_const || type.is_volatile) {
                    type_error(parameter, source, diagnostics,
                               "void must be the sole unnamed, unqualified function parameter");
                    return {};
                }
                break;
            }
            if (type.kind == CType::Kind::record) {
                type_error(parameter, source, diagnostics,
                           "by-value aggregate parameters are not supported");
                return {};
            }
            result.params.push_back(adjust_parameter(std::move(type)));
        }
        return result;
    }
    case cparser::NK_STRUCT_TYPE:
    case cparser::NK_UNION_TYPE: {
        if (!allow_incomplete) {
            type_error(node, source, diagnostics,
                       "by-value aggregate types are not supported");
            return {};
        }
        CType result;
        result.kind = CType::Kind::record;
        const auto *identity = node->getKind() == cparser::NK_STRUCT_TYPE
            ? static_cast<cparser::StructTypeASTNode *>(node)->getIdentity()
            : static_cast<cparser::UnionTypeASTNode *>(node)->getIdentity();
        result.record = identity ? static_cast<const void *>(identity) : node;
        return qualify(std::move(result));
    }
    case cparser::NK_UNKNOWN_TYPE:
        type_error(node, source, diagnostics, "unknown or incomplete parser type");
        return {};
    case cparser::NK_REAL_TYPE:
    case cparser::NK_COMPLEX_TYPE:
    case cparser::NK_REFERENCE_TYPE:
        type_error(node, source, diagnostics,
                   "floating, complex, and reference types are not supported");
        return {};
    default:
        type_error(node, source, diagnostics, "AST node is not a supported C type");
        return {};
    }
}

CType promote(const CType &type) {
    if (!type.integer())
        return {};
    CType value = unqualified(type);
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

bool compatible(const CType &left, const CType &right) {
    if (left.kind != right.kind || left.is_const != right.is_const ||
        left.is_volatile != right.is_volatile)
        return false;
    switch (left.kind) {
    case CType::Kind::invalid:
        return false;
    case CType::Kind::void_:
        return true;
    case CType::Kind::integer:
        return left.bits == right.bits && left.rank == right.rank &&
               left.is_signed == right.is_signed && left.is_plain_char == right.is_plain_char;
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
            CType promoted = parameter.integer() ? promote(parameter) : parameter;
            if (!compatible(parameter, promoted))
                return false;
        }
        return true;
    }
    return false;
}

std::optional<std::uint64_t> size_of(const CType &type) {
    switch (type.kind) {
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
    case CType::Kind::invalid:
    case CType::Kind::void_:
    case CType::Kind::function:
    case CType::Kind::record:
        return std::nullopt;
    }
    return std::nullopt;
}

unsigned align_of(const CType &type) {
    switch (type.kind) {
    case CType::Kind::integer: {
        const auto size = size_of(type);
        return size ? static_cast<unsigned>(std::min<std::uint64_t>(*size, 8)) : 0;
    }
    case CType::Kind::pointer:
        return 8;
    case CType::Kind::array:
        return type.element ? align_of(*type.element) : 0;
    case CType::Kind::invalid:
    case CType::Kind::void_:
    case CType::Kind::function:
    case CType::Kind::record:
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

std::optional<Constant> integer_constant(Node *node,
                                         const Preprocessed &source,
                                         Diagnostics &diagnostics) {
    return has_ice_form(node) ? evaluate_ice(node, source, diagnostics)
                              : std::nullopt;
}

std::optional<std::string> string_constant(Node *node,
                                           const Preprocessed &source,
                                           Diagnostics &diagnostics) {
    if (!present(node) || node->getKind() != cparser::NK_STRING_CONST)
        return std::nullopt;
    auto *literal = static_cast<cparser::StringConstASTNode *>(node);

    std::string result;
    std::string error;
    if (const auto original = spelling(literal); !original.empty()) {
        if (!decode_quoted_strings(original, result, error)) {
            diagnostics.error("cephyr.literal", error, location(source, node));
            return std::nullopt;
        }
    } else {
        // The parser stores already-decoded bytes separately from the original
        // spelling. Copy the byte string rather than getValue() so embedded
        // nulls survive and escapes are not interpreted a second time.
        result = literal->getBytes();
    }
    result.push_back('\0');
    return result;
}

} // namespace cephyr::detail
