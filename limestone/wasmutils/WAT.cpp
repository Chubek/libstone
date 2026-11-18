#include "WAT-Visitor.hpp"
#include "WATCompiler.hpp"
#include "WATParser.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace limestone::wasmutils {
namespace {

constexpr std::size_t max_nesting = 128;

enum class Kind { atom, open, close, eof };

struct Token {
    Kind kind{};
    std::size_t offset{};
    std::string_view text;
};

bool whitespace(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

class Lexer {
public:
    explicit Lexer(std::string_view input) : input_(input) {}

    Token next() {
        skip_separators();
        const auto start = cursor_;
        if (cursor_ == input_.size())
            return {Kind::eof, start, {}};
        const auto ch = input_[cursor_++];
        if (ch == '(')
            return {Kind::open, start, {}};
        if (ch == ')')
            return {Kind::close, start, {}};
        if (ch == ';') {
            if (starts(")"))
                throw Error(start, "unexpected block comment terminator");
            throw Error(start, "expected ';;' for a line comment");
        }
        while (cursor_ < input_.size()) {
            const auto c = input_[cursor_];
            if (whitespace(c) || c == '(' || c == ')' || c == ';')
                break;
            ++cursor_;
        }
        return {Kind::atom, start, input_.substr(start, cursor_ - start)};
    }

private:
    bool starts(std::string_view prefix) const noexcept {
        return input_.substr(cursor_).starts_with(prefix);
    }

    void skip_separators() {
        while (cursor_ < input_.size()) {
            if (whitespace(input_[cursor_])) {
                ++cursor_;
            } else if (starts(";;")) {
                const auto newline = input_.find_first_of("\r\n", cursor_ + 2);
                cursor_ = newline == std::string_view::npos ? input_.size() : newline;
            } else if (starts("(;")) {
                skip_block_comment();
            } else {
                break;
            }
        }
    }

    void skip_block_comment() {
        const auto start = cursor_;
        std::size_t depth = 1;
        cursor_ += 2;
        while (cursor_ < input_.size()) {
            if (starts("(;")) {
                if (depth == max_nesting)
                    throw Error(cursor_, "block comment nesting limit exceeded");
                ++depth;
                cursor_ += 2;
            } else if (starts(";)")) {
                cursor_ += 2;
                if (--depth == 0)
                    return;
            } else {
                ++cursor_;
            }
        }
        throw Error(start, "unterminated block comment");
    }

    std::string_view input_;
    std::size_t cursor_{};
};

int digit_value(char c) noexcept {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

std::int32_t parse_immediate(Token token) {
    if (token.kind != Kind::atom)
        throw Error(token.offset, "expected an integer immediate");
    auto text = token.text;
    const bool negative = text.starts_with('-');
    if (negative || text.starts_with('+'))
        text.remove_prefix(1);
    int base = 10;
    if (text.starts_with("0x")) {
        base = 16;
        text.remove_prefix(2);
    }
    const auto limit = negative ? 64 : 63;
    int value = 0;
    bool need_digit = true;
    for (const auto c : text) {
        if (c == '_' && !need_digit) {
            need_digit = true;
            continue;
        }
        const auto digit = digit_value(c);
        if (digit < 0 || digit >= base)
            throw Error(token.offset, "invalid integer immediate");
        need_digit = false;
        // Preserve lexical checking even once the literal is out of range,
        // without accumulating a value large enough to overflow.
        if (value <= limit)
            value = value * base + digit;
    }
    if (need_digit)
        throw Error(token.offset, "invalid integer immediate");
    if (value > limit)
        throw Error(token.offset, "integer immediate outside canonical range -64..63");
    return negative ? -value : value;
}

std::string token_preview(std::string_view text) {
    constexpr std::size_t max_length = 48;
    std::string preview;
    for (const auto c : text.substr(0, max_length))
        preview += c >= '!' && c <= '~' ? c : '?';
    if (text.size() > max_length)
        preview += "...";
    return preview;
}

class Parser {
public:
    explicit Parser(std::string_view input) : lexer_(input), token_(lexer_.next()) {}

    WATDocument parse() {
        WATDocument document;
        if (token_.kind == Kind::open) {
            const auto start = take().offset;
            if (atom("module"))
                return parse_module();
            document.instructions.push_back(parse_folded(start, 1));
        }
        parse_sequence(document.instructions, 1);
        require(Kind::eof, "unexpected ')' in raw instruction sequence");
        return document;
    }

private:
    bool atom(std::string_view value) const noexcept {
        return token_.kind == Kind::atom && token_.text == value;
    }

    void require(Kind kind, char const* message) const {
        if (token_.kind != kind)
            throw Error(token_.offset, message);
    }

    Token take() {
        const auto current = token_;
        token_ = lexer_.next();
        return current;
    }

    WATInstruction parse_plain() {
        require(Kind::atom, "expected a WAT instruction mnemonic");
        const auto* info = opcode_info(token_.text);
        if (!info)
            throw Error(token_.offset, "unsupported WAT instruction or construct: '" +
                                      token_preview(token_.text) + "'");
        const auto start = take().offset;
        std::int32_t immediate = 0;
        if (info->operation == Operation::constant) {
            immediate = parse_immediate(token_);
            take();
        }
        return {{info->opcode, immediate, start}, {}};
    }

    WATInstruction parse_folded(std::size_t start, std::size_t depth) {
        if (depth > max_nesting)
            throw Error(start, "folded instruction nesting limit exceeded");
        auto instruction = parse_plain();
        parse_sequence(instruction.operands, depth + 1);
        require(Kind::close, "expected ')' to close folded instruction");
        take();
        return instruction;
    }

    void parse_sequence(std::vector<WATInstruction>& instructions, std::size_t depth) {
        while (token_.kind != Kind::eof && token_.kind != Kind::close) {
            if (token_.kind == Kind::open) {
                const auto start = take().offset;
                instructions.push_back(parse_folded(start, depth));
            } else {
                instructions.push_back(parse_plain());
            }
        }
    }

    void parse_result(WATFunctionResult& result) {
        take(); // 'result'
        while (token_.kind != Kind::close) {
            if (atom("i32"))
                result.types.push_back(ValueType::i32);
            else if (atom("i64"))
                result.types.push_back(ValueType::i64);
            else
                throw Error(token_.offset, "expected 'i32', 'i64', or ')' in result declaration");
            take();
        }
        take(); // ')'
    }

    WATDocument parse_module() {
        take(); // 'module'
        constexpr auto expected_func = "expected a single anonymous '(func ...)' in module";
        require(Kind::open, expected_func);
        take();
        if (!atom("func"))
            throw Error(token_.offset, expected_func);
        take();

        WATDocument document;
        auto& result = document.function_result.emplace();
        while (token_.kind == Kind::open) {
            const auto start = take().offset;
            if (!atom("result")) {
                document.instructions.push_back(parse_folded(start, 1));
                break;
            }
            parse_result(result);
        }
        parse_sequence(document.instructions, 1);
        require(Kind::close, "expected ')' to close function");
        result.end_offset = take().offset;
        require(Kind::close, "expected ')' after single function; additional module fields are not supported");
        take();
        require(Kind::eof, "unexpected input after module");
        return document;
    }

    Lexer lexer_;
    Token token_;
};

class LoweringVisitor final : public WATVisitor {
public:
    Program program;

private:
    void visit(WATInstruction const& instruction) override {
        program.push_back(instruction.instruction);
    }
};

} // namespace

void WATVisitor::walk(WATDocument const& document) {
    struct Frame {
        std::span<const WATInstruction> pending;
        WATInstruction const* parent;
    };
    std::vector<Frame> stack{{document.instructions, nullptr}};
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.pending.empty()) {
            if (frame.parent)
                visit(*frame.parent);
            stack.pop_back();
            continue;
        }
        auto const& instruction = frame.pending.front();
        frame.pending = frame.pending.subspan(1);
        if (instruction.operands.empty())
            visit(instruction);
        else
            stack.push_back({instruction.operands, &instruction});
    }
}

WATDocument parse_wat(std::string_view input) {
    return Parser(input).parse();
}

Program compile_wat_ast(WATDocument const& document) {
    LoweringVisitor visitor;
    visitor.walk(document);
    if (document.function_result)
        validate_function(visitor.program, document.function_result->types,
                          document.function_result->end_offset);
    else
        validate(visitor.program);
    return std::move(visitor.program);
}

Program compile_wat(std::string_view input) {
    return compile_wat_ast(parse_wat(input));
}

} // namespace limestone::wasmutils
