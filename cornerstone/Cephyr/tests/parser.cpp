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

cephyr::ParsedUnit parses(std::string text) {
    cephyr::Diagnostics diagnostics;
    auto unit = cephyr::parse(source(std::move(text)), diagnostics);
    if (!unit) {
        diagnostics.render(std::cerr);
        expect(false, "valid translation unit did not parse");
    }
    expect(diagnostics.error_count() == 0, "successful parse emitted an error");
    return std::move(*unit);
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

void check_declarators() {
    auto unit = parses(R"(
        typedef int T;
        typedef int V[2][3];
        typedef int Fn(int);
        const V cv;
        V v;
        int (*rows)[2], *cols[2];
        int (*const callbacks[3])(int (*)(long), int cb(long), ...);
        Fn proto;
        int also(void), variable, (*factory(void))(long);
        int (parenthesized)(T value) { return value; }
        int prototype(int (T));
        int sizeof_types(void) {
            return sizeof(int[3]) + sizeof(int (*)[2]) + _Alignof(int (*)(void));
        }
    )");
    const auto &top = items(unit.root());
    expect(top.size() == 15, "unexpected declarator count");
    auto var_type = [&](std::size_t i) {
        return as<VarDeclASTNode>(top[i], NK_VAR_DECL)->getType();
    };
    auto element = [](ASTNode *node) {
        return as<ArrayTypeASTNode>(node, NK_ARRAY_TYPE)->getElementType();
    };
    auto pointee = [](ASTNode *node) {
        return as<PointerTypeASTNode>(node, NK_POINTER_TYPE)->getBaseType();
    };
    expect((element(element(var_type(3)))->getFlags() & TQ_CONST) != 0 &&
           (element(element(var_type(4)))->getFlags() & TQ_CONST) == 0,
           "array typedef qualifier did not apply only to the qualified declaration's elements");
    expect(element(pointee(var_type(5)))->getKind() == NK_INTEGRAL_TYPE &&
           pointee(element(var_type(6)))->getKind() == NK_INTEGRAL_TYPE,
           "parentheses did not distinguish pointer-to-array from array-of-pointer");

    auto *callback_pointer = element(var_type(7));
    expect((callback_pointer->getFlags() & TQ_CONST) != 0, "pointer qualifier was lost");
    auto *callback = as<FunctionTypeASTNode>(pointee(callback_pointer), NK_FUNCTION_TYPE);
    expect(callback->hasPrototype() && callback->isVariadic(), "nested prototype metadata was lost");
    const auto &params = items(callback->getPrms());
    expect(params.size() == 2, "wrong number of callback parameters");
    auto *anonymous = as<ParmDeclASTNode>(params[0], NK_PARM_DECL);
    expect(anonymous->getName() == NULL_AST_NODE && pointee(anonymous->getType())->getKind() == NK_FUNCTION_TYPE,
           "abstract function pointer parameter was not preserved");
    expect(as<ParmDeclASTNode>(params[1], NK_PARM_DECL)->getType()->getKind() == NK_FUNCTION_TYPE,
           "function parameter was not parsed as written");
    expect(as<FunctionDeclASTNode>(top[8], NK_FUNCTION_DECL)->hasPrototype(),
           "function typedef did not produce a prototype");
    as<FunctionDeclASTNode>(top[9], NK_FUNCTION_DECL);
    var_type(10);
    auto *factory = as<FunctionDeclASTNode>(top[11], NK_FUNCTION_DECL);
    auto *factory_type = as<FunctionTypeASTNode>(factory->getType(), NK_FUNCTION_TYPE);
    expect(pointee(factory_type->getType())->getKind() == NK_FUNCTION_TYPE,
           "function did not return a pointer to function");
    expect(!as<FunctionDeclASTNode>(top[12], NK_FUNCTION_DECL)->isForwardDeclaration(),
           "parenthesized function definition did not keep its body");
    auto *prototype = as<FunctionDeclASTNode>(top[13], NK_FUNCTION_DECL);
    auto *ambiguous = as<ParmDeclASTNode>(items(prototype->getPrms())[0], NK_PARM_DECL);
    expect(ambiguous->getName() == NULL_AST_NODE && ambiguous->getType()->getKind() == NK_FUNCTION_TYPE,
           "ambiguous parameter name did not resolve as a typedef");
    parses("int " + std::string(63, '(') + "x" + std::string(63, ')') + ";");
    parses("int f(int n, int a[sizeof(n)]); int f(int n, int a[4]) { return n; }");
}

void check_statement_scopes() {
    parses(R"(
        typedef int done;
        int f(void) {
            goto done;
            if (sizeof(enum { done = 1 })) { int x = done; }
            done: ;
            done n = 3;
            do
                (void)sizeof(enum { done = 0 });
            while (sizeof(done) != 4);
            while (n) switch (n) { default: --n; continue; }
            switch (n) { default: switch (n) { default: break; } break; }
            return n;
        }
        void g(void) { goto done; done: return; }
    )");
    auto shadow = parses("typedef int T; int f(int T, int a[sizeof(T)]) { return T; }");
    auto *f = as<FunctionDeclASTNode>(items(shadow.root())[1], NK_FUNCTION_DECL);
    auto *a = as<ParmDeclASTNode>(items(f->getPrms())[1], NK_PARM_DECL);
    auto *array = as<ArrayTypeASTNode>(a->getType(), NK_ARRAY_TYPE);
    auto *size = as<SizeOfExprASTNode>(array->getExpr(), NK_SIZEOF_EXPR);
    expect(size->getExpr()->getKind() == NK_IDENT_NODE, "parameter did not shadow a typedef in a later bound");
}

void check_namespaces() {
    auto unit = parses(R"(
        typedef int Alias;
        typedef Alias Alias;
        typedef int Matrix[3][2];
        typedef Alias Matrix[3][2];
        typedef void Callback(int [2]);
        typedef void Callback(int *const);
        typedef struct Outer Outer;
        struct Outer {
            enum Length { width = 5 } Length;
            int width;
            int data[width];
            struct Nested *next;
        };
        typedef struct Outer Outer;
        enum Length length;
        struct Nested *next;
        int local_tag(void) {
            { union Outer { int width; } *local; }
            return width;
        }
    )");
    const auto &top = items(unit.root());
    auto *outer = as<StructTypeASTNode>(top[7], NK_STRUCT_TYPE);
    const auto &fields = items(outer->getBody());
    auto *data = as<FieldDeclASTNode>(fields[2], NK_FIELD_DECL);
    auto *array = as<ArrayTypeASTNode>(data->getType(), NK_ARRAY_TYPE);
    expect(as<IntegerConstASTNode>(array->getExpr(), NK_INTEGER_CONST)->getValue() == 5,
           "member names obscured the ordinary enumerator namespace");
    auto pointee = [](ASTNode *type) {
        return as<StructTypeASTNode>(
            as<PointerTypeASTNode>(type, NK_POINTER_TYPE)->getBaseType(), NK_STRUCT_TYPE);
    };
    auto *link = as<FieldDeclASTNode>(fields[3], NK_FIELD_DECL);
    auto *global = as<VarDeclASTNode>(top[10], NK_VAR_DECL);
    expect(pointee(link->getType())->getIdentity() == pointee(global->getType())->getIdentity(),
           "tag declared within a member declaration was not visible in file scope");
}

void check_tokens() {
    parses("int asm; int NOTYPE; int a<:2:> = <%1, 2%>; int f(void) <% return a<:1:>; %>");
    parses("int a = 072; int b = 0xabcde; double c = 0x1.afp+3; char d = '\\377';");
    fails(source("int n = 0b101;"), "invalid integer constant '0b101'");
    fails(source("int n = 123_suffix;"), "invalid integer constant '123_suffix'");
    fails(source("int n = 0x;"), "invalid integer constant '0x'");
    fails(source("int n = 09;"), "invalid integer constant '09'");
    fails(source("double n = 0x1.8;"), "invalid floating constant '0x1.8'");
    fails(source("int c = '';"), "empty character constant");
    fails(source("int c = '\\400';"), "octal escape exceeds one byte");
    fails(source("char *s = \"\\400\";"), "octal escape exceeds one byte");
    auto unit = parses(R"(char *s = "\u00a3" u8"\U0001f642"; int c = '\u0040';)");
    const auto &tokens = items(unit.root());
    auto *str = as<VarDeclASTNode>(tokens[0], NK_VAR_DECL);
    auto *literal = as<StringConstASTNode>(str->getInit(), NK_STRING_CONST);
    expect(literal->getBytes() == "\xc2\xa3\xf0\x9f\x99\x82" &&
               std::string_view(literal->getSpelling()) == R"("\u00a3" u8"\U0001f642")",
           "universal character names did not preserve spelling and UTF-8 bytes");
    auto *chr = as<VarDeclASTNode>(tokens[1], NK_VAR_DECL);
    expect(as<CharConstASTNode>(chr->getInit(), NK_CHAR_CONST)->getValue() == '@',
           "allowed basic-range universal character name did not decode");
    fails(source(R"(char *s = "\u00x0";)"), "malformed universal character name");
    fails(source(R"(char *s = "\u0041";)"), "invalid code point");
    fails(source(R"(char *s = "\ud800";)"), "invalid code point");
    fails(source(R"(int c = '\U00110000';)"), "invalid code point");
    fails(source(R"(void *s = u"text";)"), "wide string literals are not supported");
    fails(source(R"(int c = L'a';)"), "wide character constants are not supported");
}

void check_extended_identifiers() {
    auto unit = parses(R"(
        typedef int \U000000C9;
        É é(É \u03B1) {
            É prefix\u0301 = α;
            struct T { É Δ; } record = { .\u0394 = prefix́ };
            goto \u03BB;
        λ:
            return record.Δ + '\u00E9' - 'é';
        }
        int \U0001D11E;
    )");
    const auto &top = items(unit.root());
    expect(top.size() == 3, "lost extended identifiers from preprocessed input");
    auto *fn = as<FunctionDeclASTNode>(top[1], NK_FUNCTION_DECL);
    expect(std::string_view(AST_IDENT_VALUE(fn->getName())) == "é",
           "wrong UTF-8 identifier in AST");
    auto *param = as<ParmDeclASTNode>(items(fn->getPrms())[0], NK_PARM_DECL);
    expect(std::string_view(AST_IDENT_VALUE(param->getName())) == "α",
           "UCN identifier was not canonicalized to UTF-8");

    fails(source(R"(int \u00p1;)"), "malformed universal character name");
    fails(source(R"(int \U000100;)"), "malformed universal character name");
    fails(source(R"(int \u0069nt;)"), "invalid code point");
    fails(source(R"(int x\uD800;)"), "invalid code point");
    fails(source(R"(int x\u0024;)"), "Annex D.1");
    fails(source(R"(int x\u1680;)"), "Annex D.1");
    fails(source(R"(int \u20D0x;)"), "Annex D.2");
    fails(source(R"(int x = 4\u00E9;)"), "extended character in numeric preprocessing token");
    fails(source("int \xf0\x8f\xbf\xbf;"), "invalid UTF-8 source encoding");
    fails(source("const char *s = \"\x80\";"), "invalid UTF-8 source encoding");
    fails(source(R"(void f(void) { \u00e9: ; é: ; })"), "duplicate label 'é'");
}

void check_predefined_function_name() {
    auto unit = parses(R"(
        int fun(void) {
            __func__[0];
            return sizeof(__func__);
        }
    )");
    auto *fn = as<FunctionDeclASTNode>(items(unit.root())[0], NK_FUNCTION_DECL);
    auto *body = as<CompoundStmtASTNode>(fn->getBody(), NK_COMPOUND_STMT);
    auto *ret = as<ReturnStmtASTNode>(items(body->getItems())[1], NK_RETURN_STMT);
    auto *size = as<SizeOfExprASTNode>(ret->getExpr(), NK_SIZEOF_EXPR);
    auto *name = as<IdentASTNode>(size->getExpr(), NK_IDENT_NODE);
    expect(std::string_view(AST_IDENT_VALUE(name)) == "__func__",
           "predefined function name should be an ordinary identifier in the AST");
    fails(source("const char *p = __func__;"), "__func__ is only available within a function body");
    fails(source("void f(void) { int __func__; }"), "cannot declare the predefined identifier __func__");
}

void check_generic_ast() {
    auto unit = parses(R"(
        typedef long LT;
        int f(void) {
            _Generic(1, int: 0);
            return _Generic((1, 2), const LT *: 3, default: _Generic(1, int: 4));
        }
    )");
    auto *fn = as<FunctionDeclASTNode>(items(unit.root())[1], NK_FUNCTION_DECL);
    auto *body = as<CompoundStmtASTNode>(fn->getBody(), NK_COMPOUND_STMT);
    as<GenericExprASTNode>(items(body->getItems())[0], NK_GENERIC_EXPR);
    auto *ret = as<ReturnStmtASTNode>(items(body->getItems())[1], NK_RETURN_STMT);
    auto *selection = as<GenericExprASTNode>(ret->getExpr(), NK_GENERIC_EXPR);
    auto *comma = as<CompoundExprASTNode>(selection->getControl(), NK_COMPOUND_EXPR);
    expect(items(comma->getElements()).size() == 2, "lost comma expression in controller");
    const auto &arms = items(selection->getAssociations());
    expect(arms.size() == 2, "lost generic association");
    auto *typed = as<GenericAssocASTNode>(arms[0], NK_GENERIC_ASSOC);
    auto *pointee = as<PointerTypeASTNode>(typed->getType(), NK_POINTER_TYPE)->getBaseType();
    expect((pointee->getFlags() & TQ_CONST) && pointee->getKind() == NK_INTEGRAL_TYPE,
           "lost qualified typedef in generic association");
    auto *fallback = as<GenericAssocASTNode>(arms[1], NK_GENERIC_ASSOC);
    expect(fallback->getType() == NULL_AST_NODE, "default association gained a type");
    as<GenericExprASTNode>(fallback->getExpr(), NK_GENERIC_EXPR);
    fails(source("int n = _Generic(0, 1: 2);"), "expected type name or default");
    fails(source("int n = _Generic(0, int: 2,);"), "expected type name or default");
}

void check_designation_ast() {
    auto unit = parses(R"(
        struct S { int member[3]; };
        struct S records[2] = { [1].member = { [2 - 1] = 4, 5 }, [0].member[2] = 7 };
    )");
    auto *records = as<VarDeclASTNode>(items(unit.root())[1], NK_VAR_DECL);
    const auto &initializers = items(records->getInit());
    expect(initializers.size() == 2, "lost designated initializer");
    auto *designated = as<DesignatedInitASTNode>(initializers[0], NK_DESIGNATED_INIT);
    const auto &path = items(designated->getDesignators());
    expect(path.size() == 2, "incorrect designation path length");
    auto *index = as<ArrayDesignatorASTNode>(path[0], NK_ARRAY_DESIGNATOR);
    expect(as<IntegerConstASTNode>(index->getIndex(), NK_INTEGER_CONST)->getValue() == 1,
           "lost array index");
    expect(std::string_view(as<IdentASTNode>(path[1], NK_IDENT_NODE)->getValue()) == "member",
           "lost member designator");
    const auto &nested = items(designated->getInit());
    expect(nested.size() == 2, "lost nested initializer boundaries");
    auto *nested_init = as<DesignatedInitASTNode>(nested[0], NK_DESIGNATED_INIT);
    auto *offset = as<ArrayDesignatorASTNode>(items(nested_init->getDesignators())[0],
                                              NK_ARRAY_DESIGNATOR);
    as<MinusExprASTNode>(offset->getIndex(), NK_MINUS_EXPR);
    as<IntegerConstASTNode>(nested[1], NK_INTEGER_CONST);
    fails(source("int a[2] = { [1] 4 };"), "expected '='");
    fails(source("int a[2] = { [] = 4 };"), "expected expression");
}

} // namespace

int main() {
    check_ast();
    check_declarators();
    check_statement_scopes();
    check_namespaces();
    check_tokens();
    check_extended_identifiers();
    check_predefined_function_name();
    check_generic_ast();
    check_designation_ast();
    auto bad = source("\nint f(void) { return @; }");
    bad.lines = {{"test.c", 1, 0}, {"included.h", 21, 0}};
    fails(std::move(bad), "included.h:21:22: error[cephyr.lex]");
    fails(source("int f(void) { int uninitialized = ; }"), "expected expression");
    fails(source("typedef int T; typedef char T;"), "cephyr.parse.semantic");
    fails(source("_Thread_local int x;"), "_Thread_local");
    fails(source("int f(void) { return \"unterminated"), "unterminated string");
    fails(source("int x; /* unterminated"), "unterminated block comment");
    fails(source("int f(void) { return " + std::string(2048, '(') + "1; }"),
          "C syntax nesting limit exceeded");
    fails(source("int " + std::string(2048, '*') + "p;"),
          "pointer declarator nesting limit exceeded");
    fails(source("int no_result(void)[3];"), "function cannot return an array or function");
    fails(source("typedef int Fn(void); Fn table[2];"), "array element type must be a complete object");
    fails(source("int p(int x, int x);"), "parameter 'x' is already declared");
    fails(source("typedef int T; int p(int T, T x);"), "expected declaration type");
    fails(source("int p(static int x);"), "register is the only storage class");
    fails(source("int p(int) { return 1; }"), "requires named parameters");
    fails(source("int p(void, int x);"), "void must be the sole unnamed");
    fails(source("restrict int x;"), "restrict requires a pointer to an object");
    fails(source("void (*restrict fp)(void);"), "restrict requires a pointer to an object");
    fails(source("typedef int T; int p(void) { return T; }"), "typedef name 'T' used as an expression");
    fails(source("int p(void) = 0;"), "function declaration cannot have an initializer");
    fails(source("typedef int T; int T(void);"), "name 'T' is already declared");
    fails(source("auto int x;"), "auto and register are not allowed at file scope");
    fails(source("int;"), "declaration must declare a declarator");
    fails(source("typedef int Rows[2]; typedef int Rows[3];"), "different declaration");
    fails(source("struct Record { int member; int member; };"), "field 'member' is already declared");
    fails(source("struct S { struct S { int n; } nested; };"), "tag 'S' is already defined");
    fails(source("enum Tag { A }; enum Tag { B };"), "tag 'Tag' is already defined");
    fails(source("struct Tag; enum Tag value;"), "tag 'Tag' refers to a different kind");
    fails(source("enum Missing *value;"), "enum tag 'Missing' needs a prior definition");
    fails(source("_Alignas(16) int x;"), "_Alignas declarations are unsupported");
    fails(source("int f(void) { break; }"), "break statement outside a loop or switch");
    fails(source("void f(void) { switch (0) { default: continue; } }"), "continue statement outside a loop");
    fails(source("void f(void) { switch (0) { default: ; default: ; } }"), "duplicate default");
    fails(source("void f(void) { case 0: ; }"), "case label outside a switch");
    fails(source("void f(void) { tag: ; { tag: ; } }"), "duplicate label 'tag'");
    fails(source("void f(void) { goto tag; } void g(void) { tag: ; }"), "goto to undefined label 'tag'");
    fails(source("void f(void) { return 1; }"), "void function cannot return a value");
    fails(source("int f(void) { return; }"), "non-void function must return a value");
    fails(source("int f(void) { for (static int i; ; ) {} }"), "for initializer can only declare auto or register");
    auto mapped_return = source("\nvoid f(void) { return 1; }");
    mapped_return.lines = {{"test.c", 1, 0}, {"included.h", 29, 0}};
    fails(std::move(mapped_return), "included.h:29:16: error[cephyr.parse.semantic]");

    cephyr::Diagnostics diagnostics;
    auto empty = cephyr::parse(source(""), diagnostics);
    expect(empty && items(empty->root()).empty(), "empty unit should parse");
    auto shadow = cephyr::parse(source(
        "typedef int T; int f(void) { T x=1; {int T=3; T++;} T y=2; return x+y; }"),
        diagnostics);
    expect(shadow.has_value(), "nested ordinary name did not shadow typedef");
}
