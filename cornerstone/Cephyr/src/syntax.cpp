#include "syntax.hpp"

#include "C11SyntaxSchema.hpp"

#include <ASTNode.h>
#include <nlohmann/json.hpp>
#include <valijson/adapters/nlohmann_json_adapter.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>
#include <yaml.h>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>

namespace cephyr::syntax {
namespace {

using Json = nlohmann::json;
using namespace cparser;

constexpr unsigned max_depth = 256;
constexpr std::size_t max_values = 1500000;

struct SyntaxError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Budget {
    std::size_t values = 0;
    void count(unsigned depth, std::size_t amount = 1) {
        if (depth > max_depth || amount > max_values - values)
            throw SyntaxError("syntax document exceeds its nesting or value limit");
        values += amount;
    }
};

// Only traverse syntax edges: scopes, type identities, and other parser
// bookkeeping are intentionally rebuilt on import.
struct Tree {
    Budget budget;

    Json node(ASTNode *ast, unsigned depth = 0) {
        budget.count(depth);
        if (!ast || ast == NULL_AST_NODE)
            return nullptr;
        Json result = Json::object();
        budget.count(depth + 1); // kind
        if (ast->getLineNum() > 0)
            result["line"] = ast->getLineNum();
        if (ast->getFlags())
            result["flags"] = ast->getFlags();

        const auto child = [&](const char *key, ASTNode *value) {
            if (!result.contains("children"))
                budget.count(depth + 1);
            result["children"][key] = node(value, depth + 2);
        };
        const auto attr = [&](const char *key, auto &&value) {
            if (!result.contains("attributes"))
                budget.count(depth + 1);
            budget.count(depth + 2);
            result["attributes"][key] = std::forward<decltype(value)>(value);
        };

#define KIND(k, name) case NK_##k: result["kind"] = name; break
#define NODE(k, name, cls, ...) \
    case NK_##k: { \
        result["kind"] = name; \
        auto *n = static_cast<cls##ASTNode *>(ast); \
        __VA_ARGS__; \
        break; \
    }
#define CHILD(key, getter) child(key, n->getter())
#define ATTR(key, getter) attr(key, n->getter())
#define UNARY(k, name, cls) NODE(k, name, cls, CHILD("expr", getExpr))
#define BINARY(k, name, cls) NODE(k, name, cls, CHILD("lhs", getLhs); CHILD("rhs", getRhs))
#define LOOP(k, name, cls) NODE(k, name, cls, CHILD("condition", getCondition); CHILD("body", getBody))
#define TAG(k, name, cls) NODE(k, name, cls, CHILD("name", getName); CHILD("body", getBody))
#define FUNCTION_TYPE CHILD("type", getType); CHILD("parameters", getPrms); \
    ATTR("prototype", hasPrototype); ATTR("variadic", isVariadic)

        switch (ast->getKind()) {
        NODE(IDENT_NODE, "identifier", Ident, ATTR("name", getValue))
        NODE(LIST, "list", Sequence,
             auto items = Json::array();
             for (auto *item : n->getElements())
                 items.push_back(node(item, depth + 2));
             result["items"] = std::move(items))
        UNARY(SIZEOF_EXPR, "sizeof", SizeOfExpr)
        UNARY(ALIGNOF_EXPR, "alignof", AlignOfExpr)
        NODE(OFFSETOF_EXPR, "offsetof", OffsetOfExpr,
             CHILD("type", getType); CHILD("designators", getDesignators))
        NODE(FUNCTION_DECL, "function_decl", FunctionDecl,
             CHILD("name", getName); CHILD("body", getBody); FUNCTION_TYPE)
        TAG(TYPE_DECL, "type_decl", TypeDecl)
        NODE(VAR_DECL, "variable_decl", VarDecl,
             CHILD("name", getName); CHILD("type", getType); CHILD("init", getInit);
             CHILD("alignments", getAlignments))
        NODE(PARM_DECL, "parameter_decl", ParmDecl, CHILD("name", getName); CHILD("type", getType))
        NODE(FIELD_DECL, "field_decl", FieldDecl,
             CHILD("name", getName); CHILD("type", getType); CHILD("alignments", getAlignments);
             CHILD("width", getWidth))
        NODE(ENUMERATOR_DECL, "enumerator_decl", EnumeratorDecl,
             ATTR("name", getName); CHILD("init", getInitializer))
        NODE(ASM_STMT, "asm", AsmStmt, ATTR("text", getData))
        KIND(BREAK_STMT, "break");
        NODE(CASE_LABEL, "case", CaseLabel, CHILD("expr", getExpr); CHILD("stmt", getStmt))
        NODE(COMPOUND_STMT, "compound_stmt", CompoundStmt, CHILD("items", getItems))
        KIND(CONTINUE_STMT, "continue");
        LOOP(DO_STMT, "do", DoStmt)
        NODE(FOR_STMT, "for", ForStmt,
             CHILD("init", getInit); CHILD("condition", getCondition); CHILD("step", getStep);
             CHILD("body", getBody))
        NODE(GOTO_STMT, "goto", GotoStmt, CHILD("label", getLabel))
        NODE(IF_STMT, "if", IfStmt,
             CHILD("condition", getCondition); CHILD("then", getThenClause); CHILD("else", getElseClause))
        NODE(LABEL_STMT, "label", LabelStmt, CHILD("label", getLabel); CHILD("stmt", getStmt))
        UNARY(RETURN_STMT, "return", ReturnStmt)
        NODE(SWITCH_STMT, "switch", SwitchStmt, CHILD("expr", getExpr); CHILD("stmt", getStmt))
        LOOP(WHILE_STMT, "while", WhileStmt)
        NODE(INTEGER_CONST, "integer", IntegerConst,
             ATTR("spelling", getSpelling); attr("value", std::to_string(n->getValue()));
             if (auto *decl = n->getEnumerator())
                 attr("enumerator", decl->getName()))
        NODE(REAL_CONST, "real", RealConst, ATTR("spelling", getSpelling))
        NODE(STRING_CONST, "string", StringConst,
             ATTR("spelling", getSpelling);
             attr("encoding", static_cast<unsigned>(n->getEncoding()));
             budget.count(depth + 3, n->getBytes().size() + n->getWideUnits().size());
             attr("bytes", std::vector<unsigned char>(n->getBytes().begin(), n->getBytes().end()));
             ATTR("wide_units", getWideUnits))
        NODE(CHAR_CONST, "character", CharConst,
             ATTR("spelling", getSpelling); ATTR("value", getValue); ATTR("single_byte", isSingleByte))
        NODE(CAST_EXPR, "cast", CastExpr, CHILD("type", getType); CHILD("expr", getExpr))
        UNARY(UNARY_PLUS_EXPR, "unary_plus", UnaryPlus)
        UNARY(BIT_NOT_EXPR, "bit_not", BitNotExpr)
        UNARY(LOG_NOT_EXPR, "logical_not", LogNotExpr)
        UNARY(PREDECREMENT_EXPR, "pre_decrement", PredecrementExpr)
        UNARY(PREINCREMENT_EXPR, "pre_increment", PreincrementExpr)
        UNARY(POSTDECREMENT_EXPR, "post_decrement", PostdecrementExpr)
        UNARY(POSTINCREMENT_EXPR, "post_increment", PostincrementExpr)
        UNARY(ADDR_EXPR, "address_of", AddrExpr)
        NODE(INDIRECT_REF, "indirect_ref", IndirectRef, CHILD("expr", getExpr); CHILD("field", getField))
        KIND(NOP_EXPR, "nop");
        BINARY(LSHIFT_EXPR, "shift_left", LShiftExpr)
        BINARY(RSHIFT_EXPR, "shift_right", RShiftExpr)
        BINARY(BIT_IOR_EXPR, "bit_or", BitIorExpr)
        BINARY(BIT_XOR_EXPR, "bit_xor", BitXorExpr)
        BINARY(BIT_AND_EXPR, "bit_and", BitAndExpr)
        BINARY(LOG_AND_EXPR, "logical_and", LogAndExpr)
        BINARY(LOG_OR_EXPR, "logical_or", LogOrExpr)
        BINARY(PLUS_EXPR, "add", PlusExpr)
        NODE(MINUS_EXPR, "subtract", MinusExpr,
             CHILD("lhs", getLhs); CHILD("rhs", getRhs); ATTR("unary", isUnary))
        BINARY(MULT_EXPR, "multiply", MultExpr)
        BINARY(TRUNC_DIV_EXPR, "divide", TruncDivExpr)
        BINARY(TRUNC_MOD_EXPR, "remainder", TruncModExpr)
        NODE(ARRAY_REF, "array_ref", ArrayRef, CHILD("expr", getExpr); CHILD("index", getIndex))
        NODE(STRUCT_REF, "member_ref", StructRef, CHILD("expr", getName); CHILD("member", getMember))
        BINARY(LT_EXPR, "less", LtExpr)
        BINARY(LE_EXPR, "less_equal", LeExpr)
        BINARY(GT_EXPR, "greater", GtExpr)
        BINARY(GE_EXPR, "greater_equal", GeExpr)
        BINARY(EQ_EXPR, "equal", EqExpr)
        BINARY(NE_EXPR, "not_equal", NeExpr)
        BINARY(ASSIGN_EXPR, "assign", AssignExpr)
        NODE(COMPOUND_EXPR, "compound_expr", CompoundExpr, CHILD("items", getElements))
        NODE(COND_EXPR, "conditional", CondExpr,
             CHILD("condition", getCondition); CHILD("then", getThenClause); CHILD("else", getElseClause))
        NODE(CALL_EXPR, "call", CallExpr, CHILD("callee", getExpr); CHILD("arguments", getArgs))
        KIND(VOID_TYPE, "void_type");
        NODE(INTEGRAL_TYPE, "integer_type", IntegralType,
             ATTR("alignment", getAlignment); ATTR("signed", getIsSigned); ATTR("rank", getRank);
             ATTR("plain_char", isPlainChar))
        NODE(REAL_TYPE, "real_type", RealType, ATTR("alignment", getAlignment); ATTR("double", getIsDouble))
        TAG(ENUMERAL_TYPE, "enum_type", EnumeralType)
        KIND(BOOLEAN_TYPE, "bool_type");
        NODE(POINTER_TYPE, "pointer_type", PointerType, CHILD("base", getBaseType))
        NODE(FUNCTION_TYPE, "function_type", FunctionType, FUNCTION_TYPE)
        NODE(ARRAY_TYPE, "array_type", ArrayType,
             CHILD("element", getElementType); CHILD("bound", getExpr);
             ATTR("bracket_qualifiers", getBracketQualifiers); ATTR("static_bound", hasStaticBound);
             ATTR("star_bound", hasStarBound))
        TAG(STRUCT_TYPE, "struct_type", StructType)
        TAG(UNION_TYPE, "union_type", UnionType)
        NODE(STATIC_ASSERT, "static_assert", StaticAssert, CHILD("expr", getExpr); CHILD("message", getMessage))
        NODE(GENERIC_EXPR, "generic", GenericExpr, CHILD("control", getControl); CHILD("associations", getAssociations))
        NODE(GENERIC_ASSOC, "generic_association", GenericAssoc, CHILD("type", getType); CHILD("expr", getExpr))
        NODE(DESIGNATED_INIT, "designated_init", DesignatedInit, CHILD("designators", getDesignators); CHILD("init", getInit))
        NODE(ARRAY_DESIGNATOR, "array_designator", ArrayDesignator, CHILD("index", getIndex))
        NODE(COMPOUND_LITERAL, "compound_literal", CompoundLiteral, CHILD("type", getType); CHILD("init", getInit))
        default:
            throw SyntaxError("cannot serialize parser node kind " + std::to_string(ast->getKind()));
        }

#undef FUNCTION_TYPE
#undef TAG
#undef LOOP
#undef BINARY
#undef UNARY
#undef ATTR
#undef CHILD
#undef NODE
#undef KIND
        return result;
    }
};

Json source_object(const Preprocessed &source) {
    auto runs = Json::array();
    const auto append = [&](unsigned physical, const Location &location) {
        runs.push_back({{"physical", physical}, {"file", location.file}, {"line", location.line}});
    };
    if (!source.line_runs.empty()) {
        for (const auto &[physical, location] : source.line_runs)
            append(physical, location);
    } else {
        for (std::size_t i = 0; i < source.lines.size(); ++i) {
            const auto &line = source.lines[i];
            if (i && line.file == source.lines[i - 1].file &&
                line.line == std::min<std::uint64_t>(std::uint64_t{source.lines[i - 1].line} + 1,
                                                    std::numeric_limits<unsigned>::max()))
                continue;
            append(static_cast<unsigned>(i + 1), line);
        }
    }
    return {{"file", source.file}, {"text", source.text}, {"line_map", std::move(runs)}};
}

void validate(const Json &document) {
    static const auto schema = [] {
        const auto definition = Json::parse(c11_syntax_schema);
        valijson::Schema result;
        valijson::SchemaParser{}.populateSchema(valijson::adapters::NlohmannJsonAdapter(definition), result);
        return result;
    }();
    // Fail fast rather than collecting a potentially large number of errors.
    if (!valijson::Validator(valijson::Validator::kStrongTypes)
            .validate(schema, valijson::adapters::NlohmannJsonAdapter(document), nullptr))
        throw SyntaxError("document does not satisfy C11Syntax.schema.json");
}

void check_values(const Json &value, Budget &budget, unsigned depth = 0) {
    budget.count(depth);
    // Valijson's adapter represents integers as int64_t. No floating-point
    // values are used by the contract; C floating literals retain spelling.
    if (value.is_number_float() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<std::int64_t>::max()))
        throw SyntaxError("expected a signed 64-bit integer in syntax document");
    if (value.is_structured())
        for (const auto &child : value)
            check_values(child, budget, depth + 1);
}

Json read_json(std::string_view text) {
    Budget budget;
    std::vector<std::set<std::string>> keys;
    return Json::parse(text, [&](int depth, Json::parse_event_t event, Json &value) {
        using Event = Json::parse_event_t;
        if (event == Event::object_start || event == Event::array_start || event == Event::value)
            budget.count(static_cast<unsigned>(depth));
        if (event == Event::object_start)
            keys.emplace_back();
        else if (event == Event::object_end)
            keys.pop_back();
        else if (event == Event::key && !keys.back().insert(value.get<std::string>()).second)
            throw SyntaxError("duplicate JSON member");
        return true;
    });
}

// YAML uses JSON-compatible scalars. Read events (not expanded nodes) so
// aliases, explicit typing, duplicate keys, and extra documents are rejected.
class YamlReader {
    struct Event {
        yaml_event_t value{};
        ~Event() { yaml_event_delete(&value); }
    };
    yaml_parser_t parser_{};
    Budget budget_;

    void next(Event &event) {
        yaml_event_delete(&event.value);
        if (!yaml_parser_parse(&parser_, &event.value))
            throw SyntaxError("invalid YAML at line " + std::to_string(parser_.problem_mark.line + 1));
    }

    static std::string_view scalar(const Event &event) {
        const auto &s = event.value.data.scalar;
        if (s.anchor || s.tag)
            throw SyntaxError("YAML anchors and tags are not allowed");
        return {reinterpret_cast<const char *>(s.value), s.length};
    }

    Json value(Event &event, unsigned depth) {
        budget_.count(depth);
        switch (event.value.type) {
        case YAML_MAPPING_START_EVENT: {
            if (event.value.data.mapping_start.anchor || event.value.data.mapping_start.tag)
                throw SyntaxError("YAML anchors and tags are not allowed");
            auto result = Json::object();
            while (true) {
                next(event);
                if (event.value.type == YAML_MAPPING_END_EVENT)
                    return result;
                if (event.value.type != YAML_SCALAR_EVENT)
                    throw SyntaxError("expected a YAML mapping key");
                auto key = std::string(scalar(event));
                if (result.contains(key))
                    throw SyntaxError("duplicate YAML mapping key");
                next(event);
                result.emplace(std::move(key), value(event, depth + 1));
            }
        }
        case YAML_SEQUENCE_START_EVENT: {
            if (event.value.data.sequence_start.anchor || event.value.data.sequence_start.tag)
                throw SyntaxError("YAML anchors and tags are not allowed");
            auto result = Json::array();
            while (true) {
                next(event);
                if (event.value.type == YAML_SEQUENCE_END_EVENT)
                    return result;
                result.push_back(value(event, depth + 1));
            }
        }
        case YAML_SCALAR_EVENT: {
            const auto text = scalar(event);
            if (event.value.data.scalar.style == YAML_PLAIN_SCALAR_STYLE) {
                if (text == "null" || text == "~" || text.empty()) return nullptr;
                if (text == "true") return true;
                if (text == "false") return false;
                // Do not silently coerce floating-point or overflowing YAML numbers.
                if (!text.empty() && (text.front() == '-' ||
                    (text.front() >= '0' && text.front() <= '9'))) {
                    auto number = Json::parse(text, nullptr, false);
                    if (number.is_number())
                        return number;
                }
            }
            return std::string(text);
        }
        default:
            throw SyntaxError("expected a YAML value (aliases are not allowed)");
        }
    }

public:
    explicit YamlReader(std::string_view text) {
        if (!yaml_parser_initialize(&parser_))
            throw std::bad_alloc();
        yaml_parser_set_input_string(&parser_, reinterpret_cast<const unsigned char *>(text.data()), text.size());
    }
    ~YamlReader() { yaml_parser_delete(&parser_); }

    Json read() {
        Event event;
        next(event); // stream start
        next(event);
        if (event.value.type != YAML_DOCUMENT_START_EVENT)
            throw SyntaxError("expected a YAML document");
        next(event);
        auto result = value(event, 0);
        next(event);
        if (event.value.type != YAML_DOCUMENT_END_EVENT)
            throw SyntaxError("expected end of YAML document");
        next(event);
        if (event.value.type != YAML_STREAM_END_EVENT)
            throw SyntaxError("expected exactly one YAML document");
        return result;
    }
};

class YamlWriter {
    yaml_emitter_t emitter_{};
    std::string text_;

    static int append(void *context, unsigned char *bytes, std::size_t size) noexcept {
        auto &text = *static_cast<std::string *>(context);
        try {
            if (size <= max_document_bytes - text.size()) {
                text.append(reinterpret_cast<const char *>(bytes), size);
                return 1;
            }
        } catch (...) {
            // An exception must not unwind through libyaml.
        }
        return 0;
    }

    void send(yaml_event_t &event, int initialized) {
        if (!initialized)
            throw SyntaxError("cannot encode YAML event");
        // libyaml owns the event even when emission fails.
        if (!yaml_emitter_emit(&emitter_, &event))
            throw SyntaxError("cannot write YAML syntax document (invalid UTF-8 or size limit)");
    }

    void scalar(std::string_view value, yaml_scalar_style_t style) {
        yaml_event_t event;
        send(event, yaml_scalar_event_initialize(&event, nullptr, nullptr,
            reinterpret_cast<const unsigned char *>(value.data()), static_cast<int>(value.size()),
            1, 1, style));
    }

    void field(const char *name, const Json &data) {
        scalar(name, YAML_PLAIN_SCALAR_STYLE);
        value(data);
    }

    void value(const Json &data) {
        yaml_event_t event;
        if (data.is_object()) {
            send(event, yaml_mapping_start_event_initialize(&event, nullptr, nullptr, 1, YAML_BLOCK_MAPPING_STYLE));
            for (auto it = data.begin(); it != data.end(); ++it)
                field(it.key().c_str(), it.value());
            send(event, yaml_mapping_end_event_initialize(&event));
        } else if (data.is_array()) {
            send(event, yaml_sequence_start_event_initialize(&event, nullptr, nullptr, 1, YAML_BLOCK_SEQUENCE_STYLE));
            for (const auto &item : data)
                value(item);
            send(event, yaml_sequence_end_event_initialize(&event));
        } else if (data.is_string()) {
            scalar(data.get_ref<const std::string &>(), YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        } else {
            scalar(data.dump(), YAML_PLAIN_SCALAR_STYLE);
        }
    }

public:
    YamlWriter() {
        if (!yaml_emitter_initialize(&emitter_))
            throw std::bad_alloc();
        yaml_emitter_set_output(&emitter_, append, &text_);
    }
    ~YamlWriter() { yaml_emitter_delete(&emitter_); }

    std::string write(const Json &document) {
        yaml_event_t event;
        send(event, yaml_stream_start_event_initialize(&event, YAML_UTF8_ENCODING));
        send(event, yaml_document_start_event_initialize(&event, nullptr, nullptr, nullptr, 1));
        send(event, yaml_mapping_start_event_initialize(&event, nullptr, nullptr, 1, YAML_BLOCK_MAPPING_STYLE));
        field("schema_version", document.at("schema_version"));
        field("ast", document.at("ast"));
        field("source", document.at("source"));
        send(event, yaml_mapping_end_event_initialize(&event));
        send(event, yaml_document_end_event_initialize(&event, 1));
        send(event, yaml_stream_end_event_initialize(&event));
        return std::move(text_);
    }
};

// A small typed S-expression encoding of the same data model. Tagging arrays
// and objects makes even empty containers unambiguous. Strings use JSON escapes.
class SexpReader {
    std::string_view text_;
    Budget budget_;

    void space() {
        const auto pos = text_.find_first_not_of(" \r\n\t");
        text_.remove_prefix(pos == text_.npos ? text_.size() : pos);
    }
    bool take(char ch) {
        space();
        if (text_.empty() || text_.front() != ch)
            return false;
        text_.remove_prefix(1);
        return true;
    }
    void expect(char ch) {
        if (!take(ch))
            throw SyntaxError(std::string("expected '") + ch + "' in S-expression");
    }
    std::string_view atom() {
        space();
        const auto token = text_.substr(0, text_.find_first_of("()\" \r\n\t"));
        if (token.empty())
            throw SyntaxError("expected S-expression atom");
        text_.remove_prefix(token.size());
        return token;
    }
    Json string() {
        space();
        if (text_.empty() || text_.front() != '"')
            throw SyntaxError("expected quoted S-expression string");
        for (std::size_t i = 1; i < text_.size(); ++i) {
            if (text_[i] == '"') {
                const auto token = text_.substr(0, i + 1);
                text_.remove_prefix(token.size());
                return Json::parse(token);
            }
            if (text_[i] == '\\')
                ++i;
        }
        throw SyntaxError("unterminated S-expression string");
    }
    Json value(unsigned depth) {
        budget_.count(depth);
        if (take('(')) {
            const auto type = atom();
            if (type == "object") {
                auto result = Json::object();
                while (!take(')')) {
                    expect('(');
                    auto key = string().get<std::string>();
                    if (result.contains(key))
                        throw SyntaxError("duplicate S-expression member");
                    result.emplace(std::move(key), value(depth + 1));
                    expect(')');
                }
                return result;
            }
            if (type == "array") {
                auto result = Json::array();
                while (!take(')'))
                    result.push_back(value(depth + 1));
                return result;
            }
            throw SyntaxError("expected object or array S-expression");
        }
        if (!text_.empty() && text_.front() == '"')
            return string();
        const auto token = atom();
        if (token == "null") return nullptr;
        if (token == "true") return true;
        if (token == "false") return false;
        std::int64_t number{};
        const auto *end = token.data() + token.size();
        const auto [ptr, error] = std::from_chars(token.data(), end, number);
        if (error == std::errc{} && ptr == end)
            return number;
        throw SyntaxError("invalid S-expression scalar");
    }

public:
    explicit SexpReader(std::string_view text) : text_(text) {}
    Json read() {
        expect('(');
        const auto version = value(1);
        auto result = value(0);
        expect(')');
        space();
        if (!text_.empty() || !result.is_object() || result.contains("schema_version"))
            throw SyntaxError("expected one versioned S-expression document");
        result["schema_version"] = version;
        return result;
    }
};

void write_sexp(const Json &value, std::string &out) {
    if (value.is_object()) {
        out += "(object";
        for (auto it = value.begin(); it != value.end(); ++it) {
            out += " (" + Json(it.key()).dump() + ' ';
            write_sexp(it.value(), out);
            out += ')';
        }
        out += ')';
    } else if (value.is_array()) {
        out += "(array";
        for (const auto &item : value) {
            out += ' ';
            write_sexp(item, out);
        }
        out += ')';
    } else {
        out += value.dump();
    }
    if (out.size() > max_document_bytes)
        throw SyntaxError("serialized syntax document exceeds 64 MiB");
}

std::string encode(cephyr_ast_format format, Json document) {
    const auto version = document.at("schema_version").dump();
    switch (format) {
    case CEPHYR_AST_JSON:
        return "{\n  \"schema_version\": " + version +
               ",\n  \"ast\": " + document.at("ast").dump() +
               ",\n  \"source\": " + document.at("source").dump() + "\n}\n";
    case CEPHYR_AST_YAML:
        return YamlWriter{}.write(document);
    case CEPHYR_AST_SEXP: {
        std::string result = "(" + version + " ";
        document.erase("schema_version");
        write_sexp(document, result);
        result += ")\n";
        return result;
    }
    default:
        throw SyntaxError("unknown AST format");
    }
}

Json decode(cephyr_ast_format format, std::string_view text) {
    switch (format) {
    case CEPHYR_AST_JSON: return read_json(text);
    case CEPHYR_AST_YAML: return YamlReader(text).read();
    case CEPHYR_AST_SEXP: return SexpReader(text).read();
    default: throw SyntaxError("unknown AST format");
    }
}

Preprocessed replay_source(Json &document) {
    auto &data = document.at("source");
    Preprocessed source;
    source.file = std::move(data.at("file").get_ref<std::string &>());
    source.text = std::move(data.at("text").get_ref<std::string &>());
    if (source.file.find('\0') != std::string::npos || source.text.find('\0') != std::string::npos)
        throw SyntaxError("source contains an embedded NUL");

    const auto lines = std::ranges::count(source.text, '\n') +
                       (!source.text.empty() && source.text.back() != '\n');
    unsigned previous = 0;
    for (auto &run : data.at("line_map")) {
        const auto physical = run.at("physical").get<unsigned>();
        if (physical <= previous || physical > lines || (!previous && physical != 1))
            throw SyntaxError("source line map must begin at line 1 and be ordered within the source");
        auto file = std::move(run.at("file").get_ref<std::string &>());
        if (file.find('\0') != std::string::npos)
            throw SyntaxError("source line map contains an embedded NUL");
        source.line_runs.emplace_back(physical, Location{std::move(file), run.at("line").get<unsigned>()});
        previous = physical;
    }
    return source;
}

} // namespace

std::optional<std::string> emit(cephyr_ast_format format, const ParsedUnit &parsed,
                                const Preprocessed &source, Diagnostics &diag) {
    try {
        Json document = {{"schema_version", CEPHYR_C11_SYNTAX_SCHEMA_VERSION},
                         {"ast", Tree{}.node(parsed.root(), 1)}, {"source", source_object(source)}};
        Budget budget;
        check_values(document, budget);
        validate(document);
        auto text = encode(format, std::move(document));
        if (text.size() > max_document_bytes)
            throw SyntaxError("serialized syntax document exceeds 64 MiB");
        return text;
    } catch (const SyntaxError &error) {
        diag.error("cephyr.ast", error.what(), Location{source.file});
    } catch (const Json::exception &) {
        diag.error("cephyr.ast", "cannot encode syntax document as UTF-8", Location{source.file});
    }
    return std::nullopt;
}

std::optional<Unit> read(cephyr_ast_format format, std::string_view text, const std::string &name,
                         Diagnostics &diag) {
    try {
        auto document = decode(format, text);
        Budget budget;
        check_values(document, budget);
        validate(document);
        auto source = replay_source(document);
        auto parsed = parse(source, diag);
        if (!parsed)
            return std::nullopt;
        if (Tree{}.node(parsed->root(), 1) != document.at("ast"))
            throw SyntaxError("serialized AST does not match its preprocessed source");
        return Unit{std::move(source), std::move(*parsed)};
    } catch (const SyntaxError &error) {
        diag.error("cephyr.ast", error.what(), Location{name});
    } catch (const Json::exception &error) {
        // Don't echo document content from parser exceptions.
        diag.error("cephyr.ast", "invalid syntax document encoding (JSON error " +
                   std::to_string(error.id) + ")", Location{name});
    }
    return std::nullopt;
}

} // namespace cephyr::syntax
