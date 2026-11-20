#include <cephyr/diagnostics.hpp>
#include <cephyr/parser.hpp>
#include <cephyr/preprocess.hpp>

#include <AbstractSyntaxTree.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

using namespace cparser;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template<class Node>
Node *as(ASTNode *node, ASTNodeKind kind) {
    expect(node && node->getKind() == kind, "unexpected AST kind");
    return static_cast<Node *>(node);
}

const auto &items(ASTNode *node) {
    return as<SequenceASTNode>(node, NK_LIST)->getElements();
}

cephyr::Preprocessed source(std::string text) {
    cephyr::Preprocessed result;
    result.text = std::move(text);
    result.file = "test.c";
    return result;
}

void fails(cephyr::Preprocessed input, std::string_view diagnostic) {
    cephyr::Diagnostics diagnostics;
    expect(!cephyr::parse(input, diagnostics), "invalid C parsed successfully");
    expect(diagnostics.error_count() == 1, "expected one diagnostic");
    std::ostringstream out;
    diagnostics.render(out);
    if (out.str().find(diagnostic) == std::string::npos) {
        std::cerr << out.str();
        expect(false, "expected parser diagnostic not found");
    }
}

void check_ast() {
    cephyr::Diagnostics diagnostics;
    auto unit = cephyr::parse(source(R"(
        typedef unsigned long long U;
        extern int proto(const unsigned *, char **, ...);
        static char *name(void);
        unsigned char *grid[2][3];
        _Static_assert(1, "file");
        static U f(int n, char **argv) {
            U x = 0XFFFFFFFFFFFFFFFFuLL;
            x += n;
            _Static_assert(1, "block");
            char q = '\x41';
            const char *str = "a\0" "b";
            return x + q + str[0] + (argv != 0);
        }
    )"), diagnostics);
    if (!unit) {
        diagnostics.render(std::cerr);
        expect(false, "valid translation unit did not parse");
    }
    auto moved = std::move(*unit);
    expect(!unit->root(), "moved-from unit still has a tree");
    const auto &top = items(moved.root());
    expect(top.size() == 6, "unexpected translation unit size");

    auto *prototype = as<FunctionDeclASTNode>(top[1], NK_FUNCTION_DECL);
    expect((prototype->getFlags() & SCS_EXTERN) && prototype->hasPrototype() &&
               prototype->isVariadic() && prototype->isForwardDeclaration(),
           "prototype metadata was lost");
    const auto &parameters = items(prototype->getPrms());
    expect(parameters.size() == 2, "incorrect prototype parameter count");
    auto *unnamed = as<ParmDeclASTNode>(parameters[0], NK_PARM_DECL);
    expect(unnamed->getName() == NULL_AST_NODE, "unexpected name for parameter");
    auto *pointer = as<PointerTypeASTNode>(unnamed->getType(), NK_POINTER_TYPE);
    auto *base = as<IntegralTypeASTNode>(pointer->getBaseType(), NK_INTEGRAL_TYPE);
    expect((base->getFlags() & TQ_CONST) && !base->getIsSigned(),
           "const unsigned pointee type was lost");

    auto *grid = as<VarDeclASTNode>(top[3], NK_VAR_DECL);
    auto *outer = as<ArrayTypeASTNode>(grid->getType(), NK_ARRAY_TYPE);
    auto *inner = as<ArrayTypeASTNode>(outer->getElementType(), NK_ARRAY_TYPE);
    expect(as<IntegerConstASTNode>(outer->getExpr(), NK_INTEGER_CONST)->getValue() == 2 &&
               as<IntegerConstASTNode>(inner->getExpr(), NK_INTEGER_CONST)->getValue() == 3 &&
               inner->getElementType()->getKind() == NK_POINTER_TYPE,
           "multidimensional array/pointer types did not bind correctly");
    auto *assertion = as<StaticAssertASTNode>(top[4], NK_STATIC_ASSERT);
    expect(assertion->getMessage()->getBytes() == "file", "lost assertion message");

    auto *function = as<FunctionDeclASTNode>(top[5], NK_FUNCTION_DECL);
    expect((function->getFlags() & SCS_STATIC) && !function->isVariadic(),
           "function lost storage class or prototype metadata");
    auto *compound = as<CompoundStmtASTNode>(function->getBody(), NK_COMPOUND_STMT);
    const auto &body = items(compound->getItems());
    expect(body.size() == 6 && body[1]->getKind() == NK_ASSIGN_EXPR &&
               body[2]->getKind() == NK_STATIC_ASSERT &&
               body[5]->getKind() == NK_RETURN_STMT,
           "block items were not preserved in source order");
    auto *x = as<VarDeclASTNode>(body[0], NK_VAR_DECL);
    auto *u = as<IntegralTypeASTNode>(x->getType(), NK_INTEGRAL_TYPE);
    expect(u->getAlignment() == 8 && u->getRank() == 5 && !u->getIsSigned(),
           "typedef lost the unsigned long long type");
    auto *integer = as<IntegerConstASTNode>(x->getInit(), NK_INTEGER_CONST);
    expect(integer->getValue() == UINT64_MAX &&
               std::string_view(integer->getSpelling()) == "0XFFFFFFFFFFFFFFFFuLL",
           "integer magnitude or spelling was lost");
    auto *q = as<VarDeclASTNode>(body[3], NK_VAR_DECL);
    auto *character = as<CharConstASTNode>(q->getInit(), NK_CHAR_CONST);
    expect(character->getValue() == 'A' &&
               std::string_view(character->getSpelling()) == "'\\x41'",
           "character spelling was lost");
    auto *str = as<VarDeclASTNode>(body[4], NK_VAR_DECL);
    auto *literal = as<StringConstASTNode>(str->getInit(), NK_STRING_CONST);
    expect(literal->getBytes() == std::string("a\0b", 3) &&
               std::string_view(literal->getSpelling()) == R"("a\0" "b")",
           "concatenated string spelling or bytes were lost");
}

} // namespace

int main() {
    check_ast();
    auto bad = source("\nint f(void) { return @; }");
    bad.lines = {{"test.c", 1, 0}, {"included.h", 21, 0}};
    fails(std::move(bad), "included.h:21:22: error[cephyr.lex]");
    fails(source("int f(void) { int uninitialized = ; }"), "expected expression");
    fails(source("typedef int T; typedef char T;"), "cephyr.parse.semantic");
    fails(source("int (*array_pointer)[2];"), "parenthesized non-function pointer");
    fails(source("int a[2] = { [1] = 4 };"), "designated initializers");
    fails(source("_Thread_local int x;"), "_Thread_local");
    fails(source("int f(void) { return \"unterminated"), "unterminated string");
    fails(source("int x; /* unterminated"), "unterminated block comment");
    fails(source("int f(void) { return " + std::string(2048, '(') + "1; }"),
          "C syntax nesting limit exceeded");
    fails(source("int " + std::string(2048, '*') + "p;"),
          "pointer declarator nesting limit exceeded");

    cephyr::Diagnostics diagnostics;
    auto empty = cephyr::parse(source(""), diagnostics);
    expect(empty && items(empty->root()).empty(), "empty unit should parse");
    auto shadow = cephyr::parse(source(
        "typedef int T; int f(void) { T x=1; {int T=3; T++;} T y=2; return x+y; }"),
        diagnostics);
    expect(shadow.has_value(), "nested ordinary name did not shadow typedef");
}
