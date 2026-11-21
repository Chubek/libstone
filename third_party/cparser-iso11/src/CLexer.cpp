// C Lexer - implementation file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#include "../include/Lexer.h"
#include "../include/CLexer.h"
#include "../../c11_source_chars.h"

#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>

namespace cparser
{

// Search of keyword
unsigned CLexer::keyword(const char *s)
{
    TokenMap::iterator it = _kwmap.find(s);

    if (it != _kwmap.end())
    {
        return it->second;
    }
    else
    {
        // Search of keyword that can be first in a statement
        it = _fstmtmap.find(s);
        return (it != _fstmtmap.end()) ? it->second : unsigned(TK_IDENT);
    }
}

static bool nameStart(int ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static bool nameDigit(int ch)
{
    return ch >= '0' && ch <= '9';
}

static void appendUTF8(std::string &value, unsigned codePoint)
{
    char encoded[4];
    const auto bytes = c11_encode_utf8(codePoint, encoded);
    value.append(encoded, bytes);
}

void CLexer::readName(Token *t)
{
    while (nameStart(_ch) || nameDigit(_ch) || _ch == '\\' || _ch >= 0x80)
    {
        if (_ch == '\\' || _ch >= 0x80)
        {
            unsigned codePoint;
            if (_ch == '\\')
            {
                t->spelling.push_back('\\');
                nextCh();
                if (_ch != 'u' && _ch != 'U')
                    error(t->line, t->col, "expected universal character name in identifier");
                t->spelling.push_back(static_cast<char>(_ch));
                codePoint = readUniversalCharacter(t, t->spelling);
            }
            else
                codePoint = readUTF8Character(t, t->spelling);

            if (!c11_identifier_ucn(codePoint))
                error(t->line, t->col, "U+%04X is not allowed in a C11 identifier (Annex D.1)",
                      codePoint);
            if (t->sval.empty() && !c11_initial_ucn(codePoint))
                error(t->line, t->col, "U+%04X is not allowed initially in a C11 identifier (Annex D.2)",
                      codePoint);
            appendUTF8(t->sval, codePoint);
        }
        else
        {
            t->spelling.push_back(static_cast<char>(_ch));
            t->sval.push_back(static_cast<char>(_ch));
            nextCh();
        }
    }

    if (t->sval == "u8" && _ch == '"')
        readStringLit(t, true);
    else if ((t->sval == "L" || t->sval == "u" || t->sval == "U") &&
             (_ch == '"' || _ch == '\''))
        error(t->line, t->col, _ch == '"' ? "wide string literals are not supported"
                                         : "wide character constants are not supported");
    else
        t->kind = keyword(t->sval.c_str());
}

void CLexer::readNumberLit(Token *t, bool leadingDot)
{
    std::string spelling;
    bool floating = leadingDot;
    bool exponentSign = false;

    if (leadingDot)
        spelling.push_back('.');

    while (_ch != EOF)
    {
        const char ch = static_cast<char>(_ch);
        if (nameStart(_ch) || nameDigit(_ch) || ch == '.')
        {
            spelling.push_back(ch);
            exponentSign = ch == 'e' || ch == 'E' || ch == 'p' || ch == 'P';
            nextCh();
        }
        else if ((ch == '+' || ch == '-') && exponentSign)
        {
            spelling.push_back(ch);
            exponentSign = false;
            nextCh();
        }
        else if (_ch == '\\' || _ch >= 0x80)
        {
            // C11 6.4.8 includes identifier-nondigit in a pp-number, but
            // neither a UCN nor an extended character can form a C number.
            error(t->line, t->col, "extended character in numeric preprocessing token");
        }
        else
        {
            break;
        }
    }

    t->spelling = spelling;
    t->sval = spelling;
    const bool hexadecimal = spelling.size() > 2 && spelling[0] == '0' &&
                             (spelling[1] == 'x' || spelling[1] == 'X');
    floating = floating || spelling.find('.') != std::string::npos ||
               (hexadecimal
                    ? spelling.find_first_of("pP") != std::string::npos
                    : spelling.find_first_of("eE") != std::string::npos ||
                      (!spelling.empty() &&
                       (spelling.back() == 'f' || spelling.back() == 'F')));
    if (floating)
    {
        const auto decimal = [](char c) { return c >= '0' && c <= '9'; };
        const auto significand = [hexadecimal, decimal](char c) {
            return decimal(c) || (hexadecimal &&
                   ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
        };
        std::size_t i = hexadecimal ? 2 : 0, digits = 0;
        bool point = false;
        while (i < spelling.size()) {
            const char ch = spelling[i];
            if (significand(ch))
                ++digits;
            else if (ch == '.' && !point)
                point = true;
            else
                break;
            ++i;
        }
        const bool hasExponent = i < spelling.size() &&
            (hexadecimal ? spelling[i] == 'p' || spelling[i] == 'P' :
                           spelling[i] == 'e' || spelling[i] == 'E');
        if (!digits || (hexadecimal ? !hasExponent : !point && !hasExponent))
            error(t->line, t->col, "invalid floating constant '%s'",
                  spelling.c_str());
        if (hasExponent) {
            ++i;
            if (i < spelling.size() && (spelling[i] == '+' || spelling[i] == '-'))
                ++i;
            const auto start = i;
            while (i < spelling.size() && decimal(spelling[i]))
                ++i;
            if (i == start)
                error(t->line, t->col, "invalid floating constant '%s'", spelling.c_str());
        }
        if (i < spelling.size() &&
            (spelling[i] == 'f' || spelling[i] == 'F' || spelling[i] == 'l' || spelling[i] == 'L'))
            ++i;
        if (i != spelling.size())
            error(t->line, t->col, "invalid floating constant '%s'",
                  spelling.c_str());
        t->kind = TK_FLOAT_LIT;
        // Legacy AST consumers keep their host approximation. Its range and
        // locale never determine the C token or Cephyr's target value.
        t->info.fval = std::strtold(spelling.c_str(), nullptr);
        return;
    }

    std::size_t suffix = spelling.size();
    while (suffix > 0)
    {
        const char ch = spelling[suffix - 1];
        if (ch != 'u' && ch != 'U' && ch != 'l' && ch != 'L')
            break;
        --suffix;
    }
    const std::string digits = spelling.substr(0, suffix);
    const std::string suffixText = spelling.substr(suffix);
    unsigned uCount = 0, lCount = 0;
    const bool matchingLongs = suffixText.find('l') == std::string::npos ||
                               suffixText.find('L') == std::string::npos;
    for (char ch : suffixText)
        (ch == 'u' || ch == 'U') ? ++uCount : ++lCount;
    if (uCount > 1 || lCount > 2 || !matchingLongs || digits.empty() ||
        (lCount == 2 && uCount == 1 &&
         (suffixText[1] == 'u' || suffixText[1] == 'U')))
        error(t->line, t->col, "invalid integer constant '%s'",
              spelling.c_str());

    // strtoull(base=0) can accept 0b in newer host libraries, but binary
    // constants are not part of C11. Decode exactly the three C11 bases.
    const bool hex = digits.size() >= 2 && digits.front() == '0' &&
                     (digits[1] == 'x' || digits[1] == 'X');
    const unsigned base = hex ? 16 : digits.front() == '0' ? 8 : 10;
    std::uint64_t value = 0;
    if (hex && digits.size() == 2)
        error(t->line, t->col, "invalid integer constant '%s'", spelling.c_str());
    for (std::size_t i = hex ? 2 : 0; i < digits.size(); ++i)
    {
        const char ch = digits[i];
        const unsigned digit = ch >= '0' && ch <= '9' ? ch - '0' :
                               ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                               ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 16;
        if (digit >= base)
            error(t->line, t->col, "invalid integer constant '%s'", spelling.c_str());
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base)
            error(t->line, t->col, "integer constant is out of 64-bit range");
        value = value * base + digit;
    }
    t->kind = TK_INT_LIT;
    t->info.uval = value;
}

unsigned CLexer::readUniversalCharacter(Token *t, std::string &spelling)
{
    const unsigned digits = _ch == 'u' ? 4 : 8;
    nextCh();
    unsigned codePoint = 0;
    for (unsigned i = 0; i < digits; ++i)
    {
        const int digit = c11_hex_value(_ch);
        if (digit < 0)
            error(t->line, t->col, "malformed universal character name");
        spelling.push_back(static_cast<char>(_ch));
        codePoint = codePoint * 16 + digit;
        nextCh();
    }
    if (!c11_valid_ucn(codePoint))
        error(t->line, t->col, "invalid code point in universal character name");
    return codePoint;
}

unsigned CLexer::readUTF8Character(Token *t, std::string &spelling)
{
    c11_utf8_state state{};
    int status;
    do
    {
        if (_ch == EOF)
            error(t->line, t->col, "truncated UTF-8 character");
        status = c11_utf8_feed(&state, static_cast<unsigned>(_ch));
        if (status < 0)
            error(t->line, t->col, "invalid UTF-8 source encoding");
        spelling.push_back(static_cast<char>(_ch));
        nextCh();
    } while (!status);
    return static_cast<unsigned>(state.value);
}

void CLexer::readStringLit(Token *t, bool utf8)
{
    t->kind = TK_STRING_LIT;
    t->sval.clear();
    t->spelling = utf8 ? "u8" : "";

    const auto hexValue = [](int ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    const auto appendEscape = [&](std::string &value, std::string &raw) {
        raw.push_back('\\');
        nextCh();
        if (_ch == EOF)
            error(t->line, t->col, "unterminated escape sequence");
        raw.push_back(static_cast<char>(_ch));
        switch (_ch)
        {
            case '\'': value.push_back('\''); nextCh(); return;
            case '"': value.push_back('"'); nextCh(); return;
            case '?': value.push_back('?'); nextCh(); return;
            case '\\': value.push_back('\\'); nextCh(); return;
            case 'a': value.push_back('\a'); nextCh(); return;
            case 'b': value.push_back('\b'); nextCh(); return;
            case 'f': value.push_back('\f'); nextCh(); return;
            case 'n': value.push_back('\n'); nextCh(); return;
            case 'r': value.push_back('\r'); nextCh(); return;
            case 't': value.push_back('\t'); nextCh(); return;
            case 'v': value.push_back('\v'); nextCh(); return;
            case 'u':
            case 'U':
                appendUTF8(value, readUniversalCharacter(t, raw));
                return;
            case 'x':
            {
                nextCh();
                int decoded = 0, count = 0;
                while (_ch != EOF && hexValue(_ch) >= 0)
                {
                    raw.push_back(static_cast<char>(_ch));
                    decoded = (decoded << 4) | hexValue(_ch);
                    if (decoded > 255)
                        error(t->line, t->col, "hex escape exceeds one byte");
                    ++count;
                    nextCh();
                }
                if (!count)
                    error(t->line, t->col, "hex escape requires a digit");
                value.push_back(static_cast<char>(decoded));
                return;
            }
            default:
                if (_ch >= '0' && _ch <= '7')
                {
                    int decoded = 0, count = 0;
                    while (count < 3 && _ch >= '0' && _ch <= '7')
                    {
                        if (count)
                            raw.push_back(static_cast<char>(_ch));
                        decoded = (decoded << 3) | (_ch - '0');
                        ++count;
                        nextCh();
                    }
                    if (decoded > 255)
                        error(t->line, t->col, "octal escape exceeds one byte");
                    value.push_back(static_cast<char>(decoded));
                    return;
                }
                error(t->line, t->col, "unsupported escape sequence");
        }
    };

    t->spelling.push_back('"');
    nextCh();
    while (_ch != '"' && _ch != EOF && _ch != '\n')
    {
        if (_ch == '\\')
            appendEscape(t->sval, t->spelling);
        else if (_ch >= 0x80)
            appendUTF8(t->sval, readUTF8Character(t, t->spelling));
        else
        {
            t->sval.push_back(static_cast<char>(_ch));
            t->spelling.push_back(static_cast<char>(_ch));
            nextCh();
        }
    }
    if (_ch != '"')
        error(t->line, t->col, "unterminated string literal");
    t->spelling.push_back('"');
    nextCh();
}

void CLexer::readCharLit(Token *t)
{
    t->spelling.clear();
    t->spelling.push_back('\'');
    nextCh();
    t->kind = TK_CHAR_LIT;
    if (_ch == '\'')
        error(t->line, t->col, "empty character constant");
    if (_ch == EOF || _ch == '\n')
        error(t->line, t->col, "unterminated character constant");

    if (_ch == '\\')
    {
        t->spelling.push_back('\\');
        nextCh();
        t->spelling.push_back(static_cast<char>(_ch));
        const auto hexValue = [](int ch) {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        bool consumed = false;
        switch (_ch)
        {
            case '0':
            {
                int value = 0, digits = 0;
                while (digits < 3 && _ch >= '0' && _ch <= '7')
                {
                    if (digits)
                        t->spelling.push_back(static_cast<char>(_ch));
                    value = (value << 3) | (_ch - '0');
                    ++digits;
                    nextCh();
                }
                consumed = true;
                t->info.ival = value;
                break;
            }
            case 'b':
                t->info.ival = 8;
                break; // Backspace
            case 'a':
                t->info.ival = 7;
                break;
            case '?':
                t->info.ival = '?';
                break;
            case 't':
                t->info.ival = 9;
                break; // Horizontal tab
            case 'n':
                t->info.ival = 10;
                break; // New line
            case 'v':
                t->info.ival = 11;
                break; // Vertical tab
            case 'f':
                t->info.ival = 12;
                break; // Form feed
            case 'r':
                t->info.ival = 13;
                break; // Carriage return
            case '\'':
                t->info.ival = 39;
                break; // Single quote
            case '\\':
                t->info.ival = 92;
                break; // Backslash
            case '"':
                t->info.ival = 34;
                break;
            case 'u':
            case 'U':
                t->info.ival = static_cast<int>(readUniversalCharacter(t, t->spelling));
                consumed = true;
                break;
            case 'x':
            {
                nextCh();
                consumed = true;
                int value = 0, digits = 0;
                while (_ch != EOF && hexValue(_ch) >= 0)
                {
                    t->spelling.push_back(static_cast<char>(_ch));
                    value = (value << 4) | hexValue(_ch);
                    if (value > 255)
                        error(t->line, t->col, "hex escape exceeds one byte");
                    ++digits;
                    nextCh();
                }
                if (!digits || value > 255)
                    error(t->line, t->col, "invalid hex character escape");
                t->info.ival = value;
                break;
            }
            default:
                if (_ch >= '1' && _ch <= '7')
                {
                    int value = 0, digits = 0;
                    while (digits < 3 && _ch >= '0' && _ch <= '7')
                    {
                        if (digits)
                            t->spelling.push_back(static_cast<char>(_ch));
                        value = (value << 3) | (_ch - '0');
                        ++digits;
                        nextCh();
                    }
                    if (value > 255)
                        error(t->line, t->col, "octal escape exceeds one byte");
                    consumed = true;
                    t->info.ival = value;
                    break;
                }
                error(t->line, t->col, "unsupported character escape");
        }
        if (!consumed)
            nextCh();
    }
    else if (_ch >= 0x80)
        t->info.ival = static_cast<int>(readUTF8Character(t, t->spelling));
    else
    {
        t->info.ival = _ch;
        t->spelling.push_back(static_cast<char>(_ch));
        nextCh();
    }

    if (_ch != '\'')
    {
        error(t->line, t->col, "unterminated or multi-character constant");
    }

    t->spelling.push_back('\'');
    nextCh();
}

void CLexer::comment(int line, int column)
{
    // The opening slash and star have already been consumed.
    bool terminated = false;
    while (_ch != EOF)
    {
        if (_ch == '*')
        {
            nextCh();
            if (_ch == '/')
            {
                nextCh();
                terminated = true;
                break;
            }
        }
        else
            nextCh();
    }
    if (!terminated)
        error(line, column, "unterminated block comment");
}

unsigned CLexer::next(Token *t)
{
nextToken:
    while (_ch != EOF && std::isspace(static_cast<unsigned char>(_ch)))
        nextCh();

    t->line = _line;
    t->col = _col;
    t->sval.clear();
    t->spelling.clear();

    if (nameStart(_ch) || _ch == '\\' || _ch >= 0x80)
    {
        readName(t);
    }
    else if (nameDigit(_ch))
    {
        readNumberLit(t);
    }
    else
    {
        switch (_ch)
        {
            case '"':
                readStringLit(t);
                break;
            case '\'':
                readCharLit(t);
                break;
            case '#':
                if (getDiagnosticHandler())
                    error(t->line, t->col, "unsupported preprocessor directive in C parser input");
                while (_ch != '\n' && _ch != EOF)
                    nextCh();
                goto nextToken;
            case '&':
                nextCh();
                if (_ch == '&')
                {
                    nextCh();
                    t->kind = TK_LOGICAL_AND;
                }
                else if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_AND_ASSIGN;
                }
                else
                {
                    t->kind = TK_AND;
                }
                break;
            case '!':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_NEQ;
                }
                else
                {
                    t->kind = TK_NOT;
                }
                break;
            case '|':
                nextCh();
                if (_ch == '|')
                {
                    nextCh();
                    t->kind = TK_LOGICAL_OR;
                }
                else if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_OR_ASSIGN;
                }
                else
                {
                    t->kind = TK_OR;
                }
                break;
            case '{':
                nextCh();
                t->kind = TK_LBRACE;
                break;
            case '}':
                nextCh();
                t->kind = TK_RBRACE;
                break;
            case '(':
                nextCh();
                t->kind = TK_LPAR;
                break;
            case ')':
                nextCh();
                t->kind = TK_RPAR;
                break;
            case '*':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_MUL_ASSIGN;
                }
                else
                {
                    t->kind = TK_TIMES;
                }
                break;
            case '+':
                nextCh();
                if (_ch == '+')
                {
                    nextCh();
                    t->kind = TK_INC_OP;
                }
                else if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_ADD_ASSIGN;
                }
                else
                {
                    t->kind = TK_PLUS;
                }
                break;
            case '-':
                nextCh();
                if (_ch == '-')
                {
                    nextCh();
                    t->kind = TK_DEC_OP;
                }
                else if (_ch == '>')
                {
                    nextCh();
                    t->kind = TK_PTR_OP;
                }
                else if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_SUB_ASSIGN;
                }
                else
                {
                    t->kind = TK_MINUS;
                }
                break;
            case '%':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_MOD_ASSIGN;
                }
                else if (_ch == '>')
                {
                    nextCh();
                    t->kind = TK_RBRACE;
                }
                else if (_ch == ':')
                {
                    nextCh();
                    if (getDiagnosticHandler())
                        error(t->line, t->col, "unsupported preprocessor directive in C parser input");
                    while (_ch != '\n' && _ch != EOF)
                        nextCh();
                    goto nextToken;
                }
                else
                {
                    t->kind = TK_MOD;
                }
                break;
            case ',':
                nextCh();
                t->kind = TK_COMMA;
                break;
            case '/':
                nextCh();
                if (_ch == '/')
                {
                    while (_ch != '\n' && _ch != EOF)
                        nextCh();
                    goto nextToken;
                }
                else if (_ch == '*')
                {
                    const int line = t->line, column = t->col;
                    nextCh();
                    comment(line, column);
                    goto nextToken;
                }
                else if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_DIV_ASSIGN;
                }
                else
                {
                    t->kind = TK_DIV;
                }
                break;
            case ';':
                nextCh();
                t->kind = TK_SEMICOLON;
                break;
            case '=':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_EQL;
                }
                else
                {
                    t->kind = TK_ASSIGN;
                }
                break;
            case '[':
                nextCh();
                t->kind = TK_LBRACK;
                break;
            case ']':
                nextCh();
                t->kind = TK_RBRACK;
                break;
            case '^':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_XOR_ASSIGN;
                }
                else
                {
                    t->kind = TK_EXCLUSIVE_OR;
                }
                break;
            case '~':
                nextCh();
                t->kind = TK_TILDA;
                break;
            case '?':
                nextCh();
                t->kind = TK_COND_OP;
                break;
            case ':':
                nextCh();
                if (_ch == '>')
                {
                    nextCh();
                    t->kind = TK_RBRACK;
                }
                else
                    t->kind = TK_COLON;
                break;
            case '<':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_LEQ;
                }
                else if (_ch == '<')
                {
                    nextCh();
                    if (_ch == '=')
                    {
                        nextCh();
                        t->kind = TK_LSHIFT_ASSIGN;
                    }
                    else
                    {
                        t->kind = TK_LSHIFT_OP;
                    }
                }
                else if (_ch == ':' || _ch == '%')
                {
                    t->kind = _ch == ':' ? TK_LBRACK : TK_LBRACE;
                    nextCh();
                }
                else
                {
                    t->kind = TK_LSS;
                }
                break;
            case '>':
                nextCh();
                if (_ch == '=')
                {
                    nextCh();
                    t->kind = TK_GEQ;
                }
                else if (_ch == '>')
                {
                    nextCh();
                    if (_ch == '=')
                    {
                        nextCh();
                        t->kind = TK_RSHIFT_ASSIGN;
                    }
                    else
                    {
                        t->kind = TK_RSHIFT_OP;
                    }
                }
                else
                {
                    t->kind = TK_GTR;
                }
                break;
            case '.':
                nextCh();
                if (_ch != EOF &&
                    std::isdigit(static_cast<unsigned char>(_ch)))
                {
                    readNumberLit(t, true);
                }
                else if (_ch == '.')
                {
                    nextCh();
                    if (_ch == '.')
                    {
                        nextCh();
                        t->kind = TK_ELLIPSIS;
                    }
                    else
                    {
                        t->kind = TK_UNKNOWN;
                    }
                }
                else
                    t->kind = TK_PERIOD;
                break;
            case EOF:
                t->kind = TK_EOF;
                break;
            default:
            {
                const int invalid = _ch;
                nextCh();
                error(t->line, t->col, "invalid character 0x%02x",
                      static_cast<unsigned>(invalid) & 0xffU);
            }
        }
    }

    return t->kind;
}

void CLexer::initialize()
{
    // Insert reserved words
    INSERT_KEYWORD("_Alignas", TK__ALIGNAS);
    INSERT_KEYWORD("_Alignof", TK__ALIGNOF);
    INSERT_KEYWORD("_Atomic", TK__ATOMIC);
    INSERT_KEYWORD("_Bool", TK__BOOL);
    INSERT_KEYWORD("_Complex", TK__COMPLEX);
    INSERT_KEYWORD("_Generic", TK__GENERIC);
    INSERT_KEYWORD("_Imaginary", TK__IMAGINARY);
    INSERT_KEYWORD("_Noreturn", TK__NORETURN);
    INSERT_KEYWORD("_Nullable", TK__NULLABLE);
    INSERT_KEYWORD("_Static_assert", TK__STATIC_ASSERT);
    INSERT_KEYWORD("_Thread_local", TK__THREAD_LOCAL);
    INSERT_KEYWORD("__func__", TK___FUNC__);

    INSERT_KEYWORD("__attribute__", TK___ATTRIBUTE__);
    INSERT_KEYWORD("__asm", TK___ASM);

    INSERT_KEYWORD("auto", TK_AUTO);
    INSERT_KEYWORD("char", TK_CHAR);
    INSERT_KEYWORD("const", TK_CONST);
    INSERT_KEYWORD("double", TK_DOUBLE);
    INSERT_KEYWORD("else", TK_ELSE);
    INSERT_KEYWORD("enum", TK_ENUM);
    INSERT_KEYWORD("extern", TK_EXTERN);
    INSERT_KEYWORD("float", TK_FLOAT);
    INSERT_KEYWORD("inline", TK_INLINE);
    INSERT_KEYWORD("int", TK_INT);
    INSERT_KEYWORD("long", TK_LONG);
    INSERT_KEYWORD("register", TK_REGISTER);
    INSERT_KEYWORD("restrict", TK_RESTRICT);
    INSERT_KEYWORD("short", TK_SHORT);
    INSERT_KEYWORD("signed", TK_SIGNED);
    INSERT_KEYWORD("sizeof", TK_SIZEOF);
    INSERT_KEYWORD("static", TK_STATIC);
    INSERT_KEYWORD("struct", TK_STRUCT);
    INSERT_KEYWORD("typedef", TK_TYPEDEF);
    INSERT_KEYWORD("union", TK_UNION);
    INSERT_KEYWORD("unsigned", TK_UNSIGNED);
    INSERT_KEYWORD("void", TK_VOID);
    INSERT_KEYWORD("volatile", TK_VOLATILE);

    INSERT_FIRST_OF_STMT_KEYWORD("_Generic", TK__GENERIC);
    INSERT_FIRST_OF_STMT_KEYWORD("break", TK_BREAK);
    INSERT_FIRST_OF_STMT_KEYWORD("case", TK_CASE);
    INSERT_FIRST_OF_STMT_KEYWORD("continue", TK_CONTINUE);
    INSERT_FIRST_OF_STMT_KEYWORD("default", TK_DEFAULT);
    INSERT_FIRST_OF_STMT_KEYWORD("do", TK_DO);
    INSERT_FIRST_OF_STMT_KEYWORD("for", TK_FOR);
    INSERT_FIRST_OF_STMT_KEYWORD("goto", TK_GOTO);
    INSERT_FIRST_OF_STMT_KEYWORD("if", TK_IF);
    INSERT_FIRST_OF_STMT_KEYWORD("return", TK_RETURN);
    INSERT_FIRST_OF_STMT_KEYWORD("switch", TK_SWITCH);
    INSERT_FIRST_OF_STMT_KEYWORD("while", TK_WHILE);
}

} // namespace cparser
