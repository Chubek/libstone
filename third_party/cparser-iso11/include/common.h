// Common stuff.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#ifndef COMMON_H
#define COMMON_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>

#define IDLEN 256
#define MAXSTR 1024

#define FAIL -1

namespace cparser
{

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

enum class DiagnosticKind { LexError, ParseError, SemanticError, Warning };
using DiagnosticHandler =
    std::function<void(DiagnosticKind, int, int, const char *)>;

// Embedders get a callback and a recoverable failure. Keep the original
// standalone formatter's exit status when it has no handler.
struct ParseFailure {};

inline void reportDiagnostic(const DiagnosticHandler &handler,
                             DiagnosticKind kind, int line, int column,
                             const char *message)
{
    if (handler)
        handler(kind, line, column, message);
    else
        std::fprintf(stderr, "%d:%d: %s: %s\n", line, column,
                     kind == DiagnosticKind::Warning ? "warning" : "error",
                     message);
}

[[noreturn]] inline void failDiagnostic(const DiagnosticHandler &handler,
                                        DiagnosticKind kind, int line,
                                        int column, const char *message)
{
    reportDiagnostic(handler, kind, line, column, message);
    if (handler)
        throw ParseFailure{};
    std::exit(EXIT_FAILURE);
}

class CharCompare
{
public:
    bool operator()(const char *a, const char *b) const
    {
        return strcmp(a, b) < 0;
    }
};

} // namespace cparser

#endif
