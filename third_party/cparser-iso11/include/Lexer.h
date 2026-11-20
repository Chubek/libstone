// Lexer - header file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#ifndef LEXER_H
#define LEXER_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>

#include "common.h"
#include <map>

namespace cparser
{

// Token type
struct Token
{
    unsigned kind;
    union
    {
        int ival;
        std::uint64_t uval;
        long double fval;
    } info;
    std::string sval;
    std::string spelling;
    int line, col;
};

typedef std::map<const char *, unsigned, CharCompare> TokenMap;

class Lexer
{
protected:
    FILE *_fp = nullptr;
    std::string _source;
    std::size_t _offset = 0;
    int _ch = 0;   // Current character
    int _line = 1; // Current line
    int _col = 0;  // Current column
    DiagnosticHandler _diagnostic;

    [[noreturn]] void error(int line, int column, const char *format, ...);

public:
    void nextCh();
    virtual unsigned next(Token *) = 0;

    Lexer(const char *filename);
    explicit Lexer(std::string source) : _source(std::move(source)) {}
    Lexer(const Lexer &) = delete;
    Lexer &operator=(const Lexer &) = delete;

    virtual ~Lexer();

    const DiagnosticHandler &getDiagnosticHandler() const { return _diagnostic; }
    void setDiagnosticHandler(DiagnosticHandler handler)
    {
        _diagnostic = std::move(handler);
    }
};

} // namespace cparser

#endif
