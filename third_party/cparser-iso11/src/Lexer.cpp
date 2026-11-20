// Lexer - implementation file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#include "../include/Lexer.h"

#include <cstdarg>
#include <cstdio>

namespace cparser
{

void Lexer::error(int line, int column, const char *format, ...)
{
    char dest[4096];

    va_list argptr;
    va_start(argptr, format);
    vsnprintf(dest, sizeof(dest), format, argptr);
    va_end(argptr);
    failDiagnostic(_diagnostic, DiagnosticKind::LexError, line, column, dest);
}

void Lexer::nextCh()
{
    if (_ch == EOF)
        return;
    if (_fp)
        _ch = getc(_fp);
    else
        _ch = _offset < _source.size()
              ? static_cast<unsigned char>(_source[_offset++]) : EOF;
    _col++;
    if (_ch == '\n')
    {
        _line++;
        _col = 0;
    }
}

Lexer::Lexer(const char *filename)
{
    _fp = filename ? fopen(filename, "r") : stdin;
    if (!_fp)
        error(1, 1, "cannot open '%s'", filename);
}

Lexer::~Lexer()
{
    if (_fp && _fp != stdin)
        fclose(_fp);
}

} // namespace cparser
