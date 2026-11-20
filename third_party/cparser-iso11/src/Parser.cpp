// Parser - implementation file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#include "../include/Parser.h"
#include "../include/Lexer.h"
#include <cassert>
#include <iostream>

namespace cparser
{

void Parser::getTok()
{
    _tokIdx = _laIdx;
    _laIdx = (_laIdx + 1) % TOK_BUF_LEN;
    _newLaIdx = (_laIdx + 2) % TOK_BUF_LEN;

    // Get the current token
    _tok = &_tokbuf[_tokIdx];

    // Get symbol type of first lookahead token
    _sym = _tokbuf[_laIdx].kind;

    // Allocate a new third lookahead token
    _lex->next(&_tokbuf[_newLaIdx]);
}

unsigned Parser::getLATok(unsigned k)
{
    assert((k < TOK_BUF_LEN) &&
           "k is greater than number of possible lookahead tokens");

    return _tokbuf[(_tokIdx + k) % TOK_BUF_LEN].kind;
}

const char *Parser::getLATokInfoSval(unsigned k)
{
    assert((k < TOK_BUF_LEN) &&
           "k is greater than number of possible lookahead tokens");

    const Token &token = _tokbuf[(_tokIdx + k) % TOK_BUF_LEN];
    return token.sval.c_str();
}

void Parser::parsingError(const char *msg)
{
    const auto &token = getLookahead();
    ++_parsingErrors;
    failDiagnostic(_lex->getDiagnosticHandler(), DiagnosticKind::ParseError,
                   token.line, token.col, msg);
}

void Parser::check(unsigned expected)
{
    if (_sym == expected)
    {
        getTok();
    }
    else
    {
        char msg[MAXSTR];

        auto name = _name.find(expected);
        std::snprintf(msg, sizeof(msg), "expected '%s'",
                      name != _name.end() ? name->second : "token");
        parsingError(msg);
    }
}

void Parser::initTokenBuffer()
{
    _sym = _lex->next(&_tokbuf[0]);
    _lex->next(&_tokbuf[1]);
    _lex->next(&_tokbuf[2]);
    _tokIdx = TOK_BUF_LEN - 1;
    _laIdx = 0;
    _newLaIdx = 2;
    _tok = &_tokbuf[_tokIdx];
}

} // namespace cparser
