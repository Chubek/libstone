#pragma once
#include "lexer.hpp"
#include <string>
#include <utility>
#include <vector>

namespace smtlib2 {
struct SExpr {
    bool is_list = false;
    TokenKind kind = TokenKind::atom;
    std::string atom;
    std::vector<SExpr> children;
    Span span;
};
/// @implstring Keep the concrete tree lossless with respect to token spelling,
/// while normalizing whitespace and dropping comments. Parsing enforces byte,
/// token, and list-depth limits; the parser does not recover from errors.
inline std::vector<SExpr> parse(std::string_view input, Limits limits = {}) {
    auto tokens = lex(input, limits); std::vector<SExpr> roots;
    std::vector<SExpr> stack;
    for (auto& t : tokens) {
        if (t.kind == TokenKind::lparen) {
            if (stack.size() >= limits.max_depth)
                throw Error(t.span, "nesting limit exceeded", ErrorCategory::resource_limit);
            SExpr e; e.is_list = true; e.span = t.span; stack.push_back(std::move(e));
        } else if (t.kind == TokenKind::rparen) {
            if (stack.empty()) throw Error(t.span, "unexpected ')'", ErrorCategory::syntax);
            SExpr e = std::move(stack.back()); stack.pop_back(); e.span.end = t.span.end;
            if (stack.empty()) roots.push_back(std::move(e)); else stack.back().children.push_back(std::move(e));
        } else {
            SExpr e; e.kind = t.kind; e.atom = std::move(t.text); e.span = t.span;
            if (stack.empty()) roots.push_back(std::move(e)); else stack.back().children.push_back(std::move(e));
        }
    }
    if (!stack.empty()) throw Error(stack.back().span, "unclosed '('", ErrorCategory::syntax);
    return roots;
}
inline std::string print(const SExpr& e) {
    if (!e.is_list) return e.atom;
    std::string result = "(";
    struct Frame { const SExpr* list; std::size_t next = 0; };
    std::vector<Frame> stack{{&e}};
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (frame.next == frame.list->children.size()) {
            result += ')';
            stack.pop_back();
            continue;
        }
        if (frame.next != 0) result += ' ';
        const auto& child = frame.list->children[frame.next++];
        if (child.is_list) {
            result += '(';
            stack.push_back({&child});
        } else {
            result += child.atom;
        }
    }
    return result;
}
}
