// C parser - implementation file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#include "../include/Parser.h"
#include "../include/CParser.h"
#include "../include/AbstractSyntaxTree.h"
#include "../include/Lexer.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <set>
#include <stack>

namespace cparser
{

// TODO: Fix the typedef

bool CParser::isTypeSpecifier(unsigned kind, int n)
{
    if (kind == TK_CHAR || kind == TK_DOUBLE || kind == TK_ENUM ||
        kind == TK_FLOAT || kind == TK_INT || kind == TK_LONG ||
         kind == TK_SHORT || kind == TK_SIGNED || kind == TK_STRUCT ||
         kind == TK_UNION || kind == TK_UNSIGNED || kind == TK_VOID ||
         kind == TK__BOOL || kind == TK__COMPLEX || kind == TK__IMAGINARY)
    {
        return true;
    }
    else if (kind == TK_IDENT)
    {
        STObject *obj = _stb.find(getLATokInfoSval(n));
        if (obj != _stb.noObj && obj->kind == STOK_TYPE)
            return true;
    }

    return false;
}

bool CParser::isTypeQualifier(unsigned kind)
{
    return kind == TK_CONST || kind == TK_VOLATILE || kind == TK_RESTRICT ||
           kind == TK__ATOMIC || kind == TK__NULLABLE;
}

bool CParser::isStorageClassSpecifier(unsigned kind)
{
    return kind == TK_TYPEDEF || kind == TK_EXTERN || kind == TK_STATIC ||
           kind == TK_AUTO || kind == TK_REGISTER || kind == TK__THREAD_LOCAL;
}

bool CParser::startsDeclaration()
{
    return isStorageClassSpecifier(_sym) || isTypeQualifier(_sym) ||
           isTypeSpecifier(_sym, 1) || _sym == TK_INLINE ||
           _sym == TK__NORETURN || _sym == TK__ALIGNAS ||
           _sym == TK__STATIC_ASSERT;
}

//
// Initialization
//

void CParser::initNames()
{
    _name.insert(std::make_pair(TK_UNKNOWN, "unknown"));
    _name.insert(std::make_pair(TK_IDENT, "identifier"));
    _name.insert(std::make_pair(TK_INT_LIT, "integer constant"));
    _name.insert(std::make_pair(TK_CHAR_LIT, "character constant"));
    _name.insert(std::make_pair(TK_FLOAT_LIT, "float constant"));
    _name.insert(std::make_pair(TK_STRING_LIT, "string constant"));
    _name.insert(std::make_pair(TK_SIZEOF, "sizeof"));
    _name.insert(std::make_pair(TK_PLUS, "+"));
    _name.insert(std::make_pair(TK_MINUS, "-"));
    _name.insert(std::make_pair(TK_TIMES, "*"));
    _name.insert(std::make_pair(TK_DIV, "/"));
    _name.insert(std::make_pair(TK_MOD, "%"));
    _name.insert(std::make_pair(TK_PERIODS, ".."));
    _name.insert(std::make_pair(TK_SEMICOLON, ";"));
    _name.insert(std::make_pair(TK_COLON, ":"));
    _name.insert(std::make_pair(TK_COMMA, ","));
    _name.insert(std::make_pair(TK_PERIOD, "."));
    _name.insert(std::make_pair(TK_LPAR, "("));
    _name.insert(std::make_pair(TK_RPAR, ")"));
    _name.insert(std::make_pair(TK_LBRACK, "["));
    _name.insert(std::make_pair(TK_RBRACK, "]"));
    _name.insert(std::make_pair(TK_LBRACE, "{"));
    _name.insert(std::make_pair(TK_RBRACE, "}"));
    _name.insert(std::make_pair(TK_TILDA, "~"));
    _name.insert(std::make_pair(TK_COND_OP, "?"));
    _name.insert(std::make_pair(TK_AND, "&"));
    _name.insert(std::make_pair(TK_LOGICAL_AND, "&&"));
    _name.insert(std::make_pair(TK_OR, "|"));
    _name.insert(std::make_pair(TK_LOGICAL_OR, "||"));
    _name.insert(std::make_pair(TK_EXCLUSIVE_OR, "^"));
    _name.insert(std::make_pair(TK_PTR_OP, "->"));
    _name.insert(std::make_pair(TK_INC_OP, "++"));
    _name.insert(std::make_pair(TK_DEC_OP, "--"));
    _name.insert(std::make_pair(TK_LSHIFT_OP, "<<"));
    _name.insert(std::make_pair(TK_RSHIFT_OP, ">>"));
    _name.insert(std::make_pair(TK_LSS, "<"));
    _name.insert(std::make_pair(TK_GTR, ">"));
    _name.insert(std::make_pair(TK_LEQ, "<="));
    _name.insert(std::make_pair(TK_GEQ, ">="));
    _name.insert(std::make_pair(TK_EQL, "=="));
    _name.insert(std::make_pair(TK_NEQ, "!="));
    _name.insert(std::make_pair(TK_NOT, "!"));
    _name.insert(std::make_pair(TK_ASSIGN, "="));
    _name.insert(std::make_pair(TK_MUL_ASSIGN, "*="));
    _name.insert(std::make_pair(TK_DIV_ASSIGN, "/="));
    _name.insert(std::make_pair(TK_MOD_ASSIGN, "%="));
    _name.insert(std::make_pair(TK_ADD_ASSIGN, "+="));
    _name.insert(std::make_pair(TK_SUB_ASSIGN, "-="));
    _name.insert(std::make_pair(TK_LSHIFT_ASSIGN, "<<"));
    _name.insert(std::make_pair(TK_RSHIFT_ASSIGN, ">>"));
    _name.insert(std::make_pair(TK_AND_ASSIGN, "&="));
    _name.insert(std::make_pair(TK_XOR_ASSIGN, "^="));
    _name.insert(std::make_pair(TK_OR_ASSIGN, "|="));
    _name.insert(std::make_pair(TK_TYPE_NAME, "type name"));
    _name.insert(std::make_pair(TK_ELLIPSIS, "..."));
    _name.insert(std::make_pair(TK_AUTO, "auto"));
    _name.insert(std::make_pair(TK_CHAR, "char"));
    _name.insert(std::make_pair(TK_CONST, "const"));
    _name.insert(std::make_pair(TK_DOUBLE, "double"));
    _name.insert(std::make_pair(TK_ELSE, "else"));
    _name.insert(std::make_pair(TK_ENUM, "enum"));
    _name.insert(std::make_pair(TK_EXTERN, "extern"));
    _name.insert(std::make_pair(TK_FLOAT, "float"));
    _name.insert(std::make_pair(TK_INT, "int"));
    _name.insert(std::make_pair(TK_LONG, "long"));
    _name.insert(std::make_pair(TK_REGISTER, "register"));
    _name.insert(std::make_pair(TK_SHORT, "short"));
    _name.insert(std::make_pair(TK_SIGNED, "signed"));
    _name.insert(std::make_pair(TK_STATIC, "static"));
    _name.insert(std::make_pair(TK_STRUCT, "struct"));
    _name.insert(std::make_pair(TK_TYPEDEF, "typedef"));
    _name.insert(std::make_pair(TK_UNION, "union"));
    _name.insert(std::make_pair(TK_UNSIGNED, "unsigned"));
    _name.insert(std::make_pair(TK_VOID, "void"));
    _name.insert(std::make_pair(TK_VOLATILE, "volatile"));
    _name.insert(std::make_pair(TK_ASM, "asm"));
    _name.insert(std::make_pair(TK_BREAK, "break"));
    _name.insert(std::make_pair(TK_CASE, "case"));
    _name.insert(std::make_pair(TK_CONTINUE, "continue"));
    _name.insert(std::make_pair(TK_DEFAULT, "default"));
    _name.insert(std::make_pair(TK_DO, "do"));
    _name.insert(std::make_pair(TK_FOR, "for"));
    _name.insert(std::make_pair(TK_GOTO, "goto"));
    _name.insert(std::make_pair(TK_IF, "if"));
    _name.insert(std::make_pair(TK_RETURN, "return"));
    _name.insert(std::make_pair(TK_SWITCH, "switch"));
    _name.insert(std::make_pair(TK_WHILE, "while"));
    _name.insert(std::make_pair(TK__STATIC_ASSERT, "_Static_assert"));
    _name.insert(std::make_pair(TK___FUNC__, "__func__"));
    _name.insert(std::make_pair(TK_EOF, "end of file"));
}

void CParser::parse(const char *)
{
    ASTNode *tree;
    // int errors;

    _lex->nextCh();
    initTokenBuffer();

    // Construct the process-wide null sentinel before enabling per-parse
    // ownership, then retain every subsequently allocated node in the AST.
    (void)NULL_AST_NODE;
    auto *previousTree = ASTNode::setAllocationTree(_ast);
    try
    {
        tree = TranslationUnit();
    }
    catch (...)
    {
        ASTNode::setAllocationTree(previousTree);
        throw;
    }
    ASTNode::setAllocationTree(previousTree);
    _ast->setRoot(tree);

    // if (!parsingErrors)
    // {
    //     if (asmOutput)
    //         _ast->getCodeGenerator()->setAsmOutput(true);
    //     errors = _ast->emitCode(output);
    // }
    // else
    // {
    //     errors = parsingErrors;
    // }

    // if (!errors)
    // {
    //     if (!_ast->getWarnings())
    //         printf("No errors; no warnings; lines: %d\n", _tok->line);
    //     else
    //         printf("No errors; warnings: %d; lines: %d\n", _ast->getWarnings(),
    //                _tok->line);
    //     if (printAST)
    //         _ast->printTree();
    //     if (printIRET)
    //         _ast->printIRExprTree();
    //     if (!asmOutput && printCodeBuffer)
    //         _ast->getCodeGenerator()->decode();
    // }
    // else
    // {
    //     printf("Errors: %d; warnings: %d\n", errors, _ast->getWarnings());
    // }
}

//
//   T H E   P A R S E R   R U L E S
//

//
// Expressions
//

ASTNode *CParser::PrimaryExpression()
{
    ASTNode *res = NULL_AST_NODE;

    if (_sym == TK_IDENT)
    {
        getTok();
        STObject *object = _stb.find(_tok->sval.c_str());
        if (object != _stb.noObj && object->kind == STOK_TYPE)
            _ast->errorAt(_tok->line, _tok->col, "typedef name '%s' used as an expression",
                          _tok->sval.c_str());
        if (object != _stb.noObj && object->kind == STOK_CON)
        {
            res = new IntegerConstASTNode(
                static_cast<std::uint64_t>(object->ival), _tok->sval);
            res->setLineNum(_tok->line);
        }
        else
            res = new IdentASTNode(_tok->sval, _tok->line);
    }
    else if (_sym == TK___FUNC__)
    {
        if (!_returnType)
            parsingError("__func__ is only available within a function body");
        getTok();
        // Treat the predefined name as an ordinary array designator. The
        // consumer supplies its implicit static const char array (6.4.2.2).
        res = new IdentASTNode(_tok->sval, _tok->line);
    }
    else if (_sym == TK_INT_LIT)
    {
        getTok();
        res = new IntegerConstASTNode(_tok->info.uval, _tok->spelling);
        res->setLineNum(_tok->line);
    }
    else if (_sym == TK_CHAR_LIT)
    {
        getTok();
        res = new CharConstASTNode(_tok->info.ival, _tok->spelling);
        res->setLineNum(_tok->line);
    }
    else if (_sym == TK_STRING_LIT)
    {
        getTok();
        res = new StringConstASTNode(_tok->sval, _tok->spelling);
        res->setLineNum(_tok->line);
    }
    else if (_sym == TK_FLOAT_LIT)
    {
        getTok();
        res = new RealConstASTNode(_tok->info.fval, _tok->spelling);
        res->setLineNum(_tok->line);
    }
    else if (_sym == TK_LPAR)
    {
        getTok();
        res = Expression();
        check(TK_RPAR);
    }
    else if (_sym == TK__GENERIC)
    {
        parsingError("_Generic selection is unsupported");
    }
    return res;
}

ASTNode *CParser::GenericSelection()
{
    check(TK__GENERIC);
    check(TK_LPAR);
    AssignmentExpression();
    check(TK_COMMA);
    GenericAssocList();
    check(TK_RPAR);
    return NULL_AST_NODE;
}

ASTNode *CParser::GenericAssocList()
{
    SequenceASTNode *lst = new SequenceASTNode(GenericAssociation());
    while (_sym == TK_COMMA)
    {
        getTok();
        lst->add(GenericAssociation());
    }
    return lst;
}

ASTNode *CParser::GenericAssociation()
{
    if (isTypeSpecifier(_sym, 1))
    {
        TypeName();
        check(TK_COLON);
        AssignmentExpression();
    }
    else if (_sym == TK_DEFAULT)
    {
        getTok();
        check(TK_COLON);
        AssignmentExpression();
    }
    else
    {
        parsingError("expected type specifier or default");
    }

    return NULL_AST_NODE;
}

//-----------------------------------------------------------------------------+
//  a.b.c->d->e                                                                |
//                                                                             |
//  =>                                                                         |
//       .                                                                     |
//      / \                                                                    |
//     a   .                                                                   |
//        / \                                                                  |
//       b  ->                                                                 |
//          / \                                                                |
//         c  ->                                                               |
//            / \                                                              |
//           d   e                                                             |
//-----------------------------------------------------------------------------+
ASTNode *CParser::parsePostfixExpression(ASTNode *expr)
{
    if (_sym == TK_PERIOD)
    {
        ASTNode *tmp;

        getTok();
        check(TK_IDENT);
        tmp = new IdentASTNode(_tok->sval, _tok->line);
        return new StructRefASTNode(expr, parsePostfixExpression(tmp));
    }
    else if (_sym == TK_PTR_OP)
    {
        ASTNode *tmp;

        getTok();
        check(TK_IDENT);
        tmp = new IdentASTNode(_tok->sval, _tok->line);
        return new IndirectRefASTNode(NULL_AST_NODE, expr,
                                      parsePostfixExpression(tmp));
    }
    else
    {
        return expr;
    }
}

ASTNode *CParser::PostfixExpression(ASTNode *typeName)
{
    ASTNode *index = NULL_AST_NODE;
    ASTNode *expr = PrimaryExpression();

    for (;;)
    {
        if (_sym == TK_LBRACK)
        {
            getTok();
            index = Expression();
            expr = new ArrayRefASTNode(typeName, expr, index);
            check(TK_RBRACK);
        }
        else if (_sym == TK_LPAR)
        {
            ASTNode *args;
            getTok();

            args = NULL_AST_NODE;
            if (_sym != TK_RPAR)
                args = ArgumentExpressionList();
            check(TK_RPAR);
            expr = new CallExprASTNode(expr, args);
        }
        else if (_sym == TK_PERIOD)
        {
            ASTNode *tmp;

            getTok();
            check(TK_IDENT);
            tmp = new IdentASTNode(_tok->sval, _tok->line);
            expr = new StructRefASTNode(expr, tmp);
        }
        else if (_sym == TK_PTR_OP)
        { // '->'
            ASTNode *tmp;

            getTok();
            check(TK_IDENT);
            tmp = new IdentASTNode(_tok->sval, _tok->line);
            expr = new IndirectRefASTNode(NULL_AST_NODE, expr, tmp);
        }
        else if (_sym == TK_INC_OP)
        { // '++'
            getTok();
            expr = new PostincrementExprASTNode(expr);
        }
        else if (_sym == TK_DEC_OP)
        { // '--'
            getTok();
            expr = new PostdecrementExprASTNode(expr);
        }
        else
        {
            break;
        }
    }
    return expr;
}

ASTNode *CParser::ArgumentExpressionList()
{
    SequenceASTNode *args = new SequenceASTNode(AssignmentExpression());
    while (_sym == TK_COMMA)
    {
        getTok();
        args->add(AssignmentExpression());
    }
    return args;
}

bool isFirstOfPostfixExpression(unsigned n)
{
    return (n >= TK_IDENT && n <= TK_STRING_LIT) ||
           n == TK_LPAR ||
           n == TK__GENERIC || n == TK___FUNC__;
}

ASTNode *CParser::UnaryExpression(ASTNode *typeName)
{
    ParseDepth depth(*this);
    ASTNode *res = NULL_AST_NODE;
    ASTNode *expr = NULL_AST_NODE;

    if (isFirstOfPostfixExpression(_sym))
    {
        res = PostfixExpression(typeName);
    }
    else if (_sym == TK_INC_OP)
    {
        getTok();
        expr = UnaryExpression(typeName);
        res = new PreincrementExprASTNode(expr);
    }
    else if (_sym == TK_DEC_OP)
    {
        getTok();
        expr = UnaryExpression(typeName);
        res = new PredecrementExprASTNode(expr);
    }
    else if (_sym == TK_AND || _sym == TK_TIMES || _sym == TK_PLUS ||
             _sym == TK_MINUS || _sym == TK_TILDA || _sym == TK_NOT)
    {
        ASTNode *type = _ast->integerTypeASTNode;
        const unsigned operation = _sym;
        getTok();
        ASTNode *operand = CastExpression();

        if (operation == TK_TIMES || operation == TK_AND)
        {
            type = new PointerTypeASTNode(_ast->integerTypeASTNode);

            if (operation == TK_AND)
                res = new AddrExprASTNode(type, operand);
            else
                res = new IndirectRefASTNode(type, operand,
                                             NULL_AST_NODE);
        }
        else if (operation == TK_TILDA)
            res = new BitNotExprASTNode(type, operand);
        else if (operation == TK_NOT)
            res = new LogNotExprASTNode(type, operand);
        else if (operation == TK_MINUS)
            res = AST_MINUS(type, new IntegerConstASTNode(0, "0"), operand);
        else
            res = new UnaryPlusASTNode(operand);
    }
    else if (_sym == TK_SIZEOF)
    {
        getTok();
        if (_sym == TK_LPAR)
        {
            if (isTypeSpecifier(getLATok(2), 2) || isTypeQualifier(getLATok(2)))
            {
                getTok();
                expr = TypeName();
                check(TK_RPAR);
            }
            else
                expr = UnaryExpression(NULL_AST_NODE);
        }
        else
        {
            expr = UnaryExpression(NULL_AST_NODE);
        }
        res = new SizeOfExprASTNode(expr);
    }
    else if (_sym == TK__ALIGNOF)
    {
        getTok();
        check(TK_LPAR);
        expr = TypeName();
        check(TK_RPAR);
        res = new AlignOfExprASTNode(expr);
    }
    else
    {
        parsingError("expected expression");
    }

    return res;
}

ASTNodeKind CParser::UnaryOperator()
{
    switch (_sym)
    {
        case TK_AND:
            getTok();
            return NK_ADDR_EXPR;
        case TK_TIMES:
            getTok();
            return NK_INDIRECT_REF;
        case TK_PLUS:
            getTok();
            return NK_UNKNOWN;
        case TK_MINUS:
            getTok();
            return NK_UNKNOWN;
        case TK_TILDA:
            getTok();
            return NK_BIT_NOT_EXPR;
        case TK_NOT:
            getTok();
            return NK_LOG_NOT_EXPR;
        default:
            return NK_UNKNOWN;
    }
}

ASTNode *CParser::CastExpression()
{
    ParseDepth depth(*this);
    ASTNode *typeName = NULL_AST_NODE;
    ASTNode *expr = NULL_AST_NODE;
    bool cast = false;

    if (_sym == TK_LPAR &&
        (isTypeSpecifier(getLATok(2), 2) || isTypeQualifier(getLATok(2))))
    {
        getTok();
        cast = true;
        typeName = TypeName();
        check(TK_RPAR);
    }
    expr = cast ? CastExpression() : UnaryExpression(typeName);
    if (cast)
    {
        return new CastExprASTNode(typeName, expr);
    }
    else
    {
        return expr;
    }
}

ASTNode *CParser::MultiplicativeExpression()
{
    ASTNode *res = CastExpression();

    for (;;)
    {
        if (_sym == TK_TIMES)
        {
            getTok();
            res = AST_MULT(_ast->integerTypeASTNode, res, CastExpression());
        }
        else if (_sym == TK_DIV)
        {
            getTok();
            res = new TruncDivExprASTNode(_ast->integerTypeASTNode, res,
                                          CastExpression());
        }
        else if (_sym == TK_MOD)
        {
            getTok();
            res = new TruncModExprASTNode(_ast->integerTypeASTNode, res,
                                          CastExpression());
        }
        else
        {
            break;
        }
    }

    res->setLineNum(_tok->line);
    return res;
}

ASTNode *CParser::AdditiveExpression()
{
    ASTNode *res = MultiplicativeExpression();

    for (;;)
    {
        if (_sym == TK_PLUS)
        {
            getTok();
            res = AST_PLUS(_ast->integerTypeASTNode, res,
                           MultiplicativeExpression());
        }
        else if (_sym == TK_MINUS)
        {
            getTok();
            res = AST_MINUS(_ast->integerTypeASTNode, res,
                            MultiplicativeExpression());
        }
        else
        {
            break;
        }
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::ShiftExpression()
{
    ASTNode *res = AdditiveExpression();

    for (;;)
    {
        if (_sym == TK_LSHIFT_OP)
        {
            getTok();
            res = new LShiftExprASTNode(_ast->integerTypeASTNode, res,
                                        AdditiveExpression());
        }
        else if (_sym == TK_RSHIFT_OP)
        {
            getTok();
            res = new RShiftExprASTNode(_ast->integerTypeASTNode, res,
                                        AdditiveExpression());
        }
        else
        {
            break;
        }
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::RelationalExpression()
{
    ASTNode *res = ShiftExpression();

    for (;;)
    {
        if (_sym == TK_LSS)
        {
            getTok();
            res = new LtExprASTNode(_ast->integerTypeASTNode, res,
                                    ShiftExpression());
        }
        else if (_sym == TK_GTR)
        {
            getTok();
            res = new GtExprASTNode(_ast->integerTypeASTNode, res,
                                    ShiftExpression());
        }
        else if (_sym == TK_LEQ)
        {
            getTok();
            res = new LeExprASTNode(_ast->integerTypeASTNode, res,
                                    ShiftExpression());
        }
        else if (_sym == TK_GEQ)
        {
            getTok();
            res = new GeExprASTNode(_ast->integerTypeASTNode, res,
                                    ShiftExpression());
        }
        else
        {
            break;
        }
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::EqualityExpression()
{
    ASTNode *res = RelationalExpression();

    for (;;)
    {
        if (_sym == TK_EQL)
        {
            getTok();
            res = new EqExprASTNode(_ast->integerTypeASTNode, res,
                                    RelationalExpression());
        }
        else if (_sym == TK_NEQ)
        {
            getTok();
            res = new NeExprASTNode(_ast->integerTypeASTNode, res,
                                    RelationalExpression());
        }
        else
        {
            break;
        }
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::AndExpression()
{
    ASTNode *res = EqualityExpression();

    while (_sym == TK_AND)
    {
        getTok();
        res = new BitAndExprASTNode(_ast->integerTypeASTNode, res,
                                    EqualityExpression());
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::ExclusiveOrExpression()
{
    ASTNode *res = AndExpression();

    while (_sym == TK_EXCLUSIVE_OR)
    {
        getTok();
        res = new BitXorExprASTNode(_ast->integerTypeASTNode, res,
                                    AndExpression());
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::InclusiveOrExpression()
{
    ASTNode *res = ExclusiveOrExpression();

    while (_sym == TK_OR)
    {
        getTok();
        res = new BitIorExprASTNode(_ast->integerTypeASTNode, res,
                                    ExclusiveOrExpression());
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::LogicalAndExpression()
{
    ASTNode *res = InclusiveOrExpression();

    while (_sym == TK_LOGICAL_AND)
    {
        getTok();
        res = new LogAndExprASTNode(_ast->integerTypeASTNode, res,
                                    InclusiveOrExpression());
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::LogicalOrExpression()
{
    ASTNode *res = LogicalAndExpression();

    while (_sym == TK_LOGICAL_OR)
    {
        getTok();
        res = new LogOrExprASTNode(_ast->integerTypeASTNode, res,
                                   LogicalAndExpression());
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::ConditionalExpression()
{
    ParseDepth depth(*this);
    ASTNode *expr = NULL_AST_NODE;
    ASTNode *res = LogicalOrExpression();

    if (_sym == TK_COND_OP)
    {
        getTok();
        expr = Expression();
        check(TK_COLON);
        res = new CondExprASTNode(res, expr, ConditionalExpression());
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::AssignmentExpression()
{
    ParseDepth depth(*this);
    ASTNode *res = ConditionalExpression();

    if (_sym >= TK_ASSIGN && _sym <= TK_OR_ASSIGN)
    {
        unsigned op = _sym;

        getTok();
        if (op >= TK_MUL_ASSIGN && op <= TK_OR_ASSIGN)
        {
            ASTNode *lhs = res;
            ASTNode *rhs = AssignmentExpression();
            ASTNode *expr = NULL_AST_NODE;

            switch (op)
            {
                case TK_MUL_ASSIGN:
                    expr = AST_MULT(_ast->integerTypeASTNode, lhs, rhs);
                    break;
                case TK_DIV_ASSIGN:
                    expr = new TruncDivExprASTNode(_ast->integerTypeASTNode,
                                                   lhs, rhs);
                    break;
                case TK_MOD_ASSIGN:
                    expr = new TruncModExprASTNode(_ast->integerTypeASTNode,
                                                   lhs, rhs);
                    break;
                case TK_ADD_ASSIGN:
                    expr = AST_PLUS(_ast->integerTypeASTNode, lhs, rhs);
                    break;
                case TK_SUB_ASSIGN:
                    expr = AST_MINUS(_ast->integerTypeASTNode, lhs, rhs);
                    break;
                case TK_LSHIFT_ASSIGN:
                    expr = new LShiftExprASTNode(_ast->integerTypeASTNode, lhs,
                                                 rhs);
                    break;
                case TK_RSHIFT_ASSIGN:
                    expr = new RShiftExprASTNode(_ast->integerTypeASTNode, lhs,
                                                 rhs);
                    break;
                case TK_AND_ASSIGN:
                    expr = new BitAndExprASTNode(_ast->integerTypeASTNode, lhs,
                                                 rhs);
                    break;
                case TK_XOR_ASSIGN:
                    expr = new BitXorExprASTNode(_ast->integerTypeASTNode, lhs,
                                                 rhs);
                    break;
                case TK_OR_ASSIGN:
                    expr = new BitIorExprASTNode(_ast->integerTypeASTNode, lhs,
                                                 rhs);
                    break;
            }
            res = new AssignExprASTNode(_ast->integerTypeASTNode, res, expr);
        }
        else
        {
            res = new AssignExprASTNode(_ast->integerTypeASTNode, res,
                                        AssignmentExpression());
        }
    }
    res->setLineNum(_tok->line);

    return res;
}

ASTNode *CParser::Expression()
{
    ASTNode *expr = AssignmentExpression();
    SequenceASTNode *expressions = nullptr;

    while (_sym == TK_COMMA)
    {
        if (!expressions)
            expressions = new SequenceASTNode(expr);
        getTok();
        expressions->add(AssignmentExpression());
    }

    return expressions ? static_cast<ASTNode *>(new CompoundExprASTNode(expressions))
                       : expr;
}

ASTNode *CParser::ConstantExpression() { return ConditionalExpression(); }

//
// Declarations
//

ASTNode *CParser::Declaration(ASTNode *declSpec)
{
    if (_sym == TK_SEMICOLON)
    {
        bool declaresName = false;
        switch (declSpec->getKind())
        {
            case NK_STRUCT_TYPE:
                declaresName = static_cast<StructTypeASTNode *>(declSpec)->getName() != NULL_AST_NODE;
                break;
            case NK_UNION_TYPE:
                declaresName = static_cast<UnionTypeASTNode *>(declSpec)->getName() != NULL_AST_NODE;
                break;
            case NK_ENUMERAL_TYPE:
                declaresName = static_cast<EnumeralTypeASTNode *>(declSpec)->getBody() != NULL_AST_NODE;
                break;
            default:
                break;
        }
        if (!declaresName)
            parsingError("declaration must declare a declarator, tag, or enumeration members");
        if (declSpec->getFlags() & (FS_INLINE | FS_NORETURN))
            parsingError("function specifier used on a non-function declaration");
    }
    ASTNode *declaration = _sym == TK_SEMICOLON ? declSpec : InitDeclaratorList(declSpec);
    check(TK_SEMICOLON);
    return declaration;
}

ASTNode *CParser::TypedefDeclaration(ASTNode *declSpec)
{
    if (declSpec->getFlags() & (FS_INLINE | FS_NORETURN))
        parsingError("function specifier is not allowed on a typedef");
    auto *types = new SequenceASTNode();
    for (;;)
    {
        ASTNode *type = declSpec;
        ASTNode *name = Declarator(type);
        auto *declaration = new TypeDeclASTNode(name, type);
        declaration->setFlags(declSpec->getFlags());
        declaration->declare(_ast);
        types->add(declaration);
        if (_sym != TK_COMMA)
            break;
        getTok();
    }
    check(TK_SEMICOLON);
    return types;
}

ASTNode *CParser::StaticAssertDeclaration()
{
    const int line = getLookahead().line;
    check(TK__STATIC_ASSERT);
    check(TK_LPAR);
    ASTNode *expr = ConstantExpression();
    check(TK_COMMA);
    check(TK_STRING_LIT);
    auto *message = new StringConstASTNode(_tok->sval, _tok->spelling);
    message->setLineNum(_tok->line);
    check(TK_RPAR);
    check(TK_SEMICOLON);
    auto *node = new StaticAssertASTNode(expr, message);
    node->setLineNum(line);
    return node;
}

static bool supportsRestrict(ASTNode *type)
{
    return type->getKind() == NK_POINTER_TYPE &&
           static_cast<PointerTypeASTNode *>(type)->getBaseType()->getKind() != NK_FUNCTION_TYPE;
}

ASTNode *CParser::DeclarationSpecifiers(bool typeOnly)
{
    ParseDepth depth(*this);
    ASTNode *declSpec = NULL_AST_NODE;
    unsigned flags = 0x0;
    unsigned longCount = 0;
    bool sawUnsigned = false, sawSigned = false;
    bool sawShort = false, sawChar = false, sawInt = false;
    bool sawVoid = false, sawFloat = false, sawDouble = false, sawBool = false;
    bool sawScalar = false;

    for (;;)
    {
        if (isStorageClassSpecifier(_sym))
        {
            if (typeOnly)
                parsingError("storage class is not allowed in a type name");
            StorageClassSpecifier(flags);
        }
        else if (isTypeQualifier(_sym))
        {
            TypeQualifier(flags);
        }
        else if (isTypeSpecifier(_sym, 1))
        {
            // A typedef name following another complete type specifier is the
            // declarator being introduced and shadows that typedef.
            if (_sym == TK_IDENT && (sawScalar || declSpec != NULL_AST_NODE))
                break;
            switch (_sym)
            {
                case TK_UNSIGNED:
                    if (sawUnsigned) parsingError("duplicate unsigned specifier");
                    sawUnsigned = sawScalar = true; getTok(); break;
                case TK_SIGNED:
                    if (sawSigned) parsingError("duplicate signed specifier");
                    sawSigned = sawScalar = true; getTok(); break;
                case TK_SHORT:
                    if (sawShort) parsingError("duplicate short specifier");
                    sawShort = sawScalar = true; getTok(); break;
                case TK_LONG: ++longCount; sawScalar = true; getTok(); break;
                case TK_CHAR:
                    if (sawChar) parsingError("duplicate char specifier");
                    sawChar = sawScalar = true; getTok(); break;
                case TK_INT:
                    if (sawInt) parsingError("duplicate int specifier");
                    sawInt = sawScalar = true; getTok(); break;
                case TK_VOID:
                    if (sawVoid) parsingError("duplicate void specifier");
                    sawVoid = sawScalar = true; getTok(); break;
                case TK_FLOAT:
                    if (sawFloat) parsingError("duplicate float specifier");
                    sawFloat = sawScalar = true; getTok(); break;
                case TK_DOUBLE:
                    if (sawDouble) parsingError("duplicate double specifier");
                    sawDouble = sawScalar = true; getTok(); break;
                case TK__BOOL:
                    if (sawBool) parsingError("duplicate _Bool specifier");
                    sawBool = sawScalar = true; getTok(); break;
                case TK__COMPLEX:
                case TK__IMAGINARY:
                    parsingError("complex and imaginary types are unsupported");
                default:
                    if (sawScalar || declSpec != NULL_AST_NODE)
                        parsingError("conflicting type specifiers");
                    declSpec = TypeSpecifier();
                     break;
            }
        }
        // else if (isTypeSpecifier(_sym, 1))
        // {
        //     TypeSpecifier();
        // }
        else if (_sym == TK___ASM || _sym == TK___ATTRIBUTE__)
        {
            parsingError("GCC declaration extensions are unsupported");
        }
        else if (_sym == TK_INLINE || _sym == TK__NORETURN)
        {
            if (typeOnly)
                parsingError("function specifier is not allowed in a type name");
            flags |= _sym == TK_INLINE ? FS_INLINE : FS_NORETURN;
            getTok();
        }
        else if (_sym == TK__ALIGNAS)
        {
            parsingError("_Alignas declarations are unsupported");
        }
        else
        {
            break;
        }
    }

    if (sawScalar)
    {
        const unsigned categoryCount = unsigned(sawChar) + unsigned(sawVoid) +
            unsigned(sawFloat) + unsigned(sawDouble) + unsigned(sawBool);
        if (sawSigned && sawUnsigned)
            parsingError("both signed and unsigned were specified");
        if (categoryCount > 1 || longCount > 2 ||
            ((sawVoid || sawFloat || sawBool) &&
             (sawSigned || sawUnsigned || sawShort || longCount || sawInt)) ||
            (sawDouble && (sawSigned || sawUnsigned || sawShort || sawInt ||
                           longCount > 1)) ||
            (sawChar && (sawShort || longCount || sawInt)) ||
            (sawShort && longCount))
            parsingError("invalid type specifier combination");

        if (sawVoid)
            declSpec = new VoidTypeASTNode();
        else if (sawFloat)
            declSpec = new RealTypeASTNode(4, false);
        else if (sawDouble)
            declSpec = new RealTypeASTNode(longCount ? 16 : 8, true);
        else if (sawBool)
            declSpec = new BooleanTypeASTNode();
        else
        {
            const unsigned bytes = sawChar ? 1 : sawShort ? 2 : longCount ? 8 : 4;
            const unsigned rank = sawChar ? 1 : sawShort ? 2 :
                                  longCount == 2 ? 5 : longCount ? 4 : 3;
            // Cephyr's only current native ABI is RV64 LP64, where plain char
            // is unsigned. All other unqualified integer types are signed.
            const bool isSigned = sawChar ? sawSigned : !sawUnsigned;
            declSpec = new IntegralTypeASTNode(bytes, isSigned, rank,
                                              sawChar && !sawSigned && !sawUnsigned);
        }
    }
    if (declSpec == NULL_AST_NODE)
        parsingError("expected declaration type");
    // Qualifying an array typedef qualifies its element type (6.7.3).
    auto *qualifiedType = declSpec;
    while (qualifiedType->getKind() == NK_ARRAY_TYPE)
        qualifiedType = static_cast<ArrayTypeASTNode *>(qualifiedType)->getElementType();
    if ((flags & DS_QUALIFIER_MASK) && qualifiedType->getKind() == NK_FUNCTION_TYPE)
        parsingError("qualified function types are unsupported");
    if ((flags & TQ_RESTRICT) && !supportsRestrict(qualifiedType))
        parsingError("restrict requires a pointer to an object type");
    qualifiedType->setFlags((qualifiedType->getFlags() & DS_QUALIFIER_MASK) |
                            (flags & DS_QUALIFIER_MASK));
    declSpec->setFlags(declSpec->getFlags() | (flags & ~DS_QUALIFIER_MASK));
    return declSpec;
}

SequenceASTNode *CParser::InitDeclaratorList(ASTNode *typeSpec)
{
    SequenceASTNode *initlist = new SequenceASTNode(InitDeclarator(typeSpec));

    while (_sym == TK_COMMA)
    {
        getTok();
        initlist->add(InitDeclarator(typeSpec));
    }

    return initlist;
}

static ASTNode *cloneType(ASTNode *type)
{
    ASTNode *copy = type;
    switch (type->getKind())
    {
        case NK_VOID_TYPE:
            copy = new VoidTypeASTNode();
            break;
        case NK_BOOLEAN_TYPE:
            copy = new BooleanTypeASTNode();
            break;
        case NK_INTEGRAL_TYPE:
        {
            auto *integer = static_cast<IntegralTypeASTNode *>(type);
            copy = new IntegralTypeASTNode(integer->getAlignment(),
                                           integer->getIsSigned(),
                                           integer->getRank(),
                                           integer->isPlainChar());
            break;
        }
        case NK_REAL_TYPE:
        {
            auto *real = static_cast<RealTypeASTNode *>(type);
            copy = new RealTypeASTNode(real->getAlignment(), real->getIsDouble());
            break;
        }
        case NK_POINTER_TYPE:
            copy = new PointerTypeASTNode(
                cloneType(static_cast<PointerTypeASTNode *>(type)->getBaseType()));
            break;
        case NK_ARRAY_TYPE:
        {
            auto *array = static_cast<ArrayTypeASTNode *>(type);
            copy = new ArrayTypeASTNode(cloneType(array->getElementType()),
                                        array->getExpr());
            break;
        }
        case NK_FUNCTION_TYPE:
        {
            auto *function = static_cast<FunctionTypeASTNode *>(type);
            auto *signature = new FunctionTypeASTNode(function->getType(),
                                                       function->getPrms());
            signature->setPrototype(function->hasPrototype());
            signature->setVariadic(function->isVariadic());
            copy = signature;
            break;
        }
        case NK_STRUCT_TYPE:
        {
            auto *record = static_cast<StructTypeASTNode *>(type);
            copy = new StructTypeASTNode(record->getName(), record->getBody(),
                                         record->getIdentity());
            break;
        }
        case NK_UNION_TYPE:
        {
            auto *record = static_cast<UnionTypeASTNode *>(type);
            copy = new UnionTypeASTNode(record->getName(), record->getBody(),
                                        record->getIdentity());
            break;
        }
        case NK_ENUMERAL_TYPE:
        {
            auto *enumeration = static_cast<EnumeralTypeASTNode *>(type);
            copy = new EnumeralTypeASTNode(enumeration->getName(),
                                           enumeration->getBody(),
                                           enumeration->getIdentity());
            break;
        }
        default:
            break;
    }
    copy->setFlags(type->getFlags());
    copy->setLineNum(type->getLineNum());
    return copy;
}

ASTNode *CParser::InitDeclarator(ASTNode *typeSpec)
{
    ASTNode *declaratorType = typeSpec;
    STScope *parameterScope = nullptr;
    ASTNode *name = Declarator(declaratorType, DeclaratorName::Required,
                               &parameterScope);
    return InitDeclaratorAfterName(typeSpec, declaratorType, name, parameterScope);
}

ASTNode *CParser::InitDeclaratorAfterName(ASTNode *typeSpec, ASTNode *type,
                                         ASTNode *name, STScope *parameterScope,
                                         bool allowDefinition)
{
    const unsigned flags = typeSpec->getFlags();
    if (type->getKind() == NK_FUNCTION_TYPE)
        return FunctionDeclaration(static_cast<FunctionTypeASTNode *>(type),
                                   name, flags, parameterScope, allowDefinition);
    if (type->getKind() == NK_VOID_TYPE)
        _ast->error(name->getLineNum(), "object cannot have void type");
    ASTNode *init = NULL_AST_NODE;
    if (flags & (FS_INLINE | FS_NORETURN))
        parsingError("function specifier used on a non-function declaration");
    VarDeclASTNode *declr = new VarDeclASTNode(type, name);
    declr->setFlags(typeSpec->getFlags());
    declr->declare(_ast);

    if (_sym == TK_ASSIGN)
    {
        getTok();
        init = Initializer();
        // if (declr->getKind() == NK_VAR_DECL)
        // {
        //     VarDeclASTNode *varDecl = static_cast<VarDeclASTNode *>(declr);
        //     varDecl->setInit(init);
        // }
        declr->setInit(init);
    }

    return declr;
}

void CParser::StorageClassSpecifier(unsigned &flags)
{
    if (flags & DS_STORAGE_MASK)
        parsingError("multiple storage-class specifiers are unsupported");
    if (_sym == TK_TYPEDEF)
    {
        getTok();
        flags |= SCS_TYPEDEF;
    }
    else if (_sym == TK_EXTERN)
    {
        getTok();
        flags |= SCS_EXTERN;
    }
    else if (_sym == TK_STATIC)
    {
        getTok();
        flags |= SCS_STATIC;
    }
    else if (_sym == TK__THREAD_LOCAL)
    {
        parsingError("_Thread_local storage is unsupported");
    }
    else if (_sym == TK_AUTO)
    {
        getTok();
        flags |= SCS_AUTO;
    }
    else if (_sym == TK_REGISTER)
    {
        getTok();
        flags |= SCS_REGISTER;
    }
}

static ASTNode *
stbTypeToASTNodeType(AbstractSyntaxTree *ast, STType *type, const char *objName)
{
    switch (type->kind)
    {
        case STTK_NONE:
            return NULL_AST_NODE;
        case STTK_CHAR:
            return new IntegralTypeASTNode(type->size, type->isSigned, 1);
        case STTK_SHORT:
            return new IntegralTypeASTNode(type->size, type->isSigned, 2);
        case STTK_INT:
            return new IntegralTypeASTNode(type->size, type->isSigned, 3);
        case STTK_UNSIGNED:
            return new IntegralTypeASTNode(type->size, false,
                                           type->size > 4 ? 4 : 3);
        case STTK_LONG:
            return new IntegralTypeASTNode(type->size, type->isSigned, 4);
        case STTK_FLOAT:
            return ast->floatTypeASTNode;
        case STTK_DOUBLE:
            return new RealTypeASTNode(type->size, true);
        case STTK_VOID:
            return ast->voidTypeASTNode;
        case STTK_ARRAY:
            //  return IR_i32;
            return NULL_AST_NODE;
        case STTK_STRUCT:
            // return _ast->createStructTypeASTNode(new IdentASTNode(objName),
            //                                    NULL_AST_NODE);
            return new StructTypeASTNode(new IdentASTNode(
                                             type->tagName.empty()
                                                 ? objName
                                                 : type->tagName),
                                          NULL_AST_NODE);
        case STTK_UNION:
            return new UnionTypeASTNode(new IdentASTNode(
                                            type->tagName.empty()
                                                ? objName
                                                : type->tagName),
                                        NULL_AST_NODE);
        case STTK_BOOL:
            return new BooleanTypeASTNode();
        case STTK_REAL:
            return NULL_AST_NODE;
        case STTK_POINTER:
            return new PointerTypeASTNode(stbTypeToASTNodeType(ast,
                                                               type->baseType,
                                                               objName));
        case STTK_ENUM:
            return new EnumeralTypeASTNode(new IdentASTNode(
                                               type->tagName.empty()
                                                   ? objName
                                                   : type->tagName),
                                           NULL_AST_NODE);
        case STTK_FUNCTION:
            return NULL_AST_NODE;
    }
    return NULL_AST_NODE;
}

ASTNode *CParser::TypeSpecifier()
{
    if (_sym == TK_VOID)
    {
        getTok();
        return _ast->voidTypeASTNode;
    }
    else if (_sym == TK_CHAR)
    {
        getTok();
        return _ast->charTypeASTNode;
    }
    else if (_sym == TK_SHORT)
    {
        getTok();
        return _ast->shortTypeASTNode;
    }
    else if (_sym == TK_INT)
    {
        getTok();
        return _ast->integerTypeASTNode;
    }
    else if (_sym == TK_LONG)
    {
        getTok();
        return _ast->longTypeASTNode;
    }
    else if (_sym == TK_FLOAT)
    {
        getTok();
        return _ast->floatTypeASTNode;
    }
    else if (_sym == TK_DOUBLE)
    {
        getTok();
    }
    else if (_sym == TK_SIGNED)
    {
        getTok();
        return new IntegralTypeASTNode(4, true, 3);
    }
    else if (_sym == TK_UNSIGNED)
    {
        getTok();
        return _ast->unsignedTypeASTNode;
    }
    else if (_sym == TK__BOOL)
    {
        getTok();
        return new BooleanTypeASTNode();
    }
    else if (_sym == TK__COMPLEX || _sym == TK__IMAGINARY)
    {
        parsingError("complex and imaginary types are unsupported");
    }
    else if (_sym == TK_STRUCT || _sym == TK_UNION)
    {
        return StructOrUnionSpecifier();
    }
    else if (_sym == TK_ENUM)
    {
        return EnumSpecifier();
    }
    else if (_sym == TK_IDENT)
    { // TYPE_NAME
        char msg[MAXSTR];
        STObject *obj;

        getTok();

        obj = _stb.find(_tok->sval.c_str());
        if (obj != _stb.noObj)
        {
            if (obj->astType)
                return cloneType(obj->astType);
            return stbTypeToASTNodeType(_ast, obj->type, obj->name.c_str());
        }

        std::snprintf(msg, sizeof(msg), "unknown type '%s'", _tok->sval.c_str());
        parsingError(msg);
    }

    return NULL_AST_NODE;
}

// TODO: Make distinct productions for a struct type declaration and for
//       declaration of variable of a struct type.
ASTNode *CParser::StructOrUnionSpecifier()
{
    ASTNode *typeName = NULL_AST_NODE;
    ASTNode *typeBody = NULL_AST_NODE;
    STType *recordType = nullptr;
    ASTNodeKind typeKind;

    if (_sym == TK_STRUCT)
    {
        getTok();
        typeKind = NK_STRUCT_TYPE;
    }
    else
    { // _sym == UNION
        getTok();
        typeKind = NK_UNION_TYPE;
    }

    const auto recordKind = typeKind == NK_STRUCT_TYPE ? STTK_STRUCT : STTK_UNION;
    if (_sym == TK_IDENT)
    {
        getTok();
        typeName = new IdentASTNode(_tok->sval, _tok->line);
        recordType = TagType(*_tok, recordKind, _sym == TK_LBRACE, _sym == TK_SEMICOLON);
    }
    else if (_sym == TK_LBRACE)
        recordType = _stb.allocType(recordKind);
    else
        parsingError("expected a tag or record definition");

    if (_sym == TK_LBRACE)
    {
        getTok();
        typeBody = StructDeclarationList();
        check(TK_RBRACE);

        // Member names have a namespace per record. Parsing the declaration
        // list must not introduce a new lexical scope for tags or enumerators.
        std::set<std::string> members;
        STObject **tail = &recordType->fields;
        for (auto *member : static_cast<SequenceASTNode *>(typeBody)->getElements())
        {
            auto *field = static_cast<FieldDeclASTNode *>(member);
            auto *name = field->getName();
            if (field->getType()->getKind() == NK_VOID_TYPE ||
                field->getType()->getKind() == NK_FUNCTION_TYPE)
                _ast->error(name->getLineNum(), "record member must have an object type");
            auto *type = _ast->getStbType(field->getType());
            if (!type)
                type = _stb.noType;
            const char *spelling = AST_IDENT_VALUE(name);
            if (!members.emplace(spelling).second)
                _ast->error(name->getLineNum(), "field '%s' is already declared", spelling);
            auto *object = _stb.allocObject(spelling, STOK_VAR, type);
            *tail = object;
            tail = &object->next;
            if (typeKind == NK_STRUCT_TYPE)
                recordType->size += type->size;
            else if (recordType->size < type->size)
                recordType->size = type->size;
        }
        recordType->nFields = static_cast<unsigned>(members.size());
        recordType->defining = false;
        recordType->complete = true;
    }
    if (typeKind == NK_STRUCT_TYPE)
        return new StructTypeASTNode(typeName, typeBody, recordType);
    return new UnionTypeASTNode(typeName, typeBody, recordType);
}

STType *CParser::TagType(const Token &tag, STTypeKind kind, bool defining,
                       bool forward)
{
    const std::string key = "tag:" + tag.sval;
    const auto *found = defining || forward ? _stb.findLocal(key.c_str()) :
                                              _stb.find(key.c_str());
    if (found == _stb.noObj)
    {
        if (kind == STTK_ENUM && !defining)
            _ast->errorAt(tag.line, tag.col, "enum tag '%s' needs a prior definition",
                          tag.sval.c_str());
        auto *type = _stb.allocType(kind);
        type->tagName = tag.sval;
        type->defining = defining;
        _stb.insert(key.c_str(), STOK_TYPE, type);
        return type;
    }
    auto *type = found->type;
    if (type->kind != kind)
        _ast->errorAt(tag.line, tag.col, "tag '%s' refers to a different kind of type",
                      tag.sval.c_str());
    if (defining && (type->complete || type->defining))
        _ast->errorAt(tag.line, tag.col, "tag '%s' is already defined", tag.sval.c_str());
    if (kind == STTK_ENUM && !defining && !type->complete)
        _ast->errorAt(tag.line, tag.col, "enum tag '%s' needs a prior definition",
                      tag.sval.c_str());
    if (defining)
        type->defining = true;
    return type;
}

SequenceASTNode *CParser::StructDeclarationList()
{
    SequenceASTNode *declist = new SequenceASTNode(StructDeclaration());

    while (_sym != TK_RBRACE)
        declist->add(StructDeclaration());

    return declist;
}

ASTNode *CParser::StructDeclaration()
{
    ASTNode *typeSpec = SpecifierQualifierList();
    SequenceASTNode *declaratorList = StructDeclaratorList(typeSpec);
    check(TK_SEMICOLON);

    return declaratorList;
}

ASTNode *CParser::SpecifierQualifierList()
{
    return DeclarationSpecifiers(true);
}

SequenceASTNode *CParser::StructDeclaratorList(ASTNode *typeSpec)
{
    SequenceASTNode *declist = new SequenceASTNode(StructDeclarator(typeSpec));

    while (_sym == TK_COMMA)
    {
        getTok();
        declist->add(StructDeclarator(typeSpec));
    }

    return declist;
}

ASTNode *CParser::StructDeclarator(ASTNode *typeSpec)
{
    ASTNode *name = Declarator(typeSpec);
    return new FieldDeclASTNode(typeSpec, name);
}

ASTNode *CParser::EnumSpecifier()
{
    ASTNode *name = NULL_AST_NODE;
    ASTNode *body = NULL_AST_NODE;

    getTok(); // Eat 'enum'.

    STType *type = nullptr;
    if (_sym == TK_IDENT)
    {
        getTok();
        name = new IdentASTNode(_tok->sval, _tok->line);
        type = TagType(*_tok, STTK_ENUM, _sym == TK_LBRACE, false);
    }
    else if (_sym != TK_LBRACE)
        parsingError("expected a tag or enumeration definition");
    else
        type = _stb.allocType(STTK_ENUM);

    if (_sym == TK_LBRACE)
    {
        getTok();
        body = EnumeratorList();
        check(TK_RBRACE);
        type->defining = false;
        type->complete = true;
    }

    return new EnumeralTypeASTNode(name, body, type);
}

SequenceASTNode *CParser::EnumeratorList()
{
    auto *enums = new SequenceASTNode();
    std::int64_t nextValue = 0;
    for (;;)
    {
        check(TK_IDENT);
        const int line = _tok->line, column = _tok->col;
        const std::string name = _tok->sval;
        enums->add(new IdentASTNode(name, line));
        std::int64_t value = nextValue;
        if (_sym == TK_ASSIGN)
        {
            getTok();
            const bool negative = _sym == TK_MINUS;
            if (negative)
                getTok();
            const bool isEnumerator = _sym == TK_IDENT;
            ASTNode *expr = ConstantExpression();
            if (AST_MATCH_INTEGER_CONST(expr))
            {
                const auto magnitude = AST_INTEGER_CONST_VALUE(expr);
                if (isEnumerator)
                    value = static_cast<std::int32_t>(magnitude);
                else if (magnitude > std::uint64_t(INT32_MAX) + unsigned(negative))
                    _ast->errorAt(line, column, "enumerator '%s' exceeds int range",
                                  name.c_str());
                else
                    value = static_cast<std::int64_t>(magnitude);
            }
            else if (expr->getKind() == NK_CHAR_CONST)
                value = static_cast<CharConstASTNode *>(expr)->getValue();
            else
                _ast->errorAt(line, column,
                              "enumerator '%s' requires a literal or previous enumerator",
                              name.c_str());
            if (negative)
                value = -value;
        }
        if (value < INT32_MIN || value > INT32_MAX)
            _ast->errorAt(line, column, "enumerator '%s' exceeds int range",
                          name.c_str());
        auto *obj = _stb.insert(name.c_str(), STOK_CON, _stb.intType);
        if (obj == _stb.noObj)
            _ast->errorAt(line, column, "enumerator '%s' is already declared",
                          name.c_str());
        obj->ival = static_cast<int>(value);
        nextValue = value + 1;

        if (_sym != TK_COMMA)
            break;
        getTok();
        if (_sym == TK_RBRACE)
            break;
    }

    return enums;
}

void CParser::TypeQualifier(unsigned &flags)
{
    if (_sym == TK_CONST)
    {
        getTok();
        flags |= TQ_CONST;
    }
    else if (_sym == TK_RESTRICT)
    {
        getTok();
        flags |= TQ_RESTRICT;
    }
    else if (_sym == TK_VOLATILE)
    {
        getTok();
        flags |= TQ_VOLATILE;
    }
    else if (_sym == TK__ATOMIC)
    {
        parsingError("_Atomic types are unsupported");
    }
    else if (_sym == TK__NULLABLE)
    {
        parsingError("_Nullable types are unsupported");
    }
    else
    {
        parsingError("expected type qualifier");
    }
}

void CParser::FunctionSpecifier()
{
    if (_sym == TK_INLINE)
    {
        getTok();
    }
    else if (_sym == TK__NORETURN)
    {
        getTok();
    }
    else
    {
        parsingError("expected function specifier");
    }
}

ASTNode *CParser::Declarator(ASTNode * &typeSpec, DeclaratorName mode,
                            STScope **parameterScope)
{
    std::vector<DeclaratorPart> parts;
    ASTNode *name = DeclaratorParts(parts, mode);
    if (parameterScope)
        *parameterScope = parts.empty() ? nullptr : parts.front().scope;

    // The parts are ordered from the identifier outward. Construct the type
    // in reverse: '*a[2]' gives array(pointer(T)), '(*a)[2]' pointer(array(T)).
    for (auto part = parts.rbegin(); part != parts.rend(); ++part)
    {
        const auto base = typeSpec->getKind();
        switch (part->kind)
        {
            case DeclaratorPart::Pointer:
                typeSpec = new PointerTypeASTNode(typeSpec);
                if ((part->qualifiers & TQ_RESTRICT) && !supportsRestrict(typeSpec))
                    _ast->errorAt(part->line, part->column,
                                  "restrict requires a pointer to an object type");
                typeSpec->setFlags(part->qualifiers);
                break;
            case DeclaratorPart::Array:
                if (base == NK_VOID_TYPE || base == NK_FUNCTION_TYPE ||
                    (base == NK_ARRAY_TYPE &&
                     static_cast<ArrayTypeASTNode *>(typeSpec)->getExpr() == NULL_AST_NODE))
                    _ast->errorAt(part->line, part->column,
                                  "array element type must be a complete object type");
                typeSpec = new ArrayTypeASTNode(typeSpec, part->operand);
                break;
            case DeclaratorPart::Function:
            {
                if (base == NK_ARRAY_TYPE || base == NK_FUNCTION_TYPE)
                    _ast->errorAt(part->line, part->column,
                                  "function cannot return an array or function type");
                auto *function = new FunctionTypeASTNode(typeSpec, part->operand);
                function->setPrototype(part->operand != NULL_AST_NODE);
                function->setVariadic(part->variadic);
                typeSpec = function;
                break;
            }
        }
        typeSpec->setLineNum(part->line);
    }
    if (_sym == TK___ASM || _sym == TK___ATTRIBUTE__)
        parsingError("GCC declarator extensions are unsupported");
    return name;
}

ASTNode *CParser::DeclaratorParts(std::vector<DeclaratorPart> &parts,
                                 DeclaratorName mode)
{
    ParseDepth depth(*this);
    std::vector<DeclaratorPart> pointers;
    while (_sym == TK_TIMES)
    {
        if (pointers.size() == 128)
            parsingError("pointer declarator nesting limit exceeded");
        const auto &token = getLookahead();
        DeclaratorPart pointer{DeclaratorPart::Pointer, token.line, token.col};
        getTok();
        while (isTypeQualifier(_sym))
            TypeQualifier(pointer.qualifiers);
        pointers.push_back(pointer);
    }

    ASTNode *name = NULL_AST_NODE;
    if (_sym == TK___FUNC__ && mode != DeclaratorName::Forbidden)
        parsingError("cannot declare the predefined identifier __func__");
    if (_sym == TK_IDENT && mode != DeclaratorName::Forbidden)
    {
        getTok();
        name = new IdentASTNode(_tok->sval, _tok->line);
    }
    else if (_sym == TK_LPAR &&
             (mode == DeclaratorName::Required ||
              (getLATok(2) != TK_RPAR &&
               !isTypeSpecifier(getLATok(2), 2) &&
               !isTypeQualifier(getLATok(2)) &&
               !isStorageClassSpecifier(getLATok(2)) &&
               getLATok(2) != TK_INLINE && getLATok(2) != TK__NORETURN &&
               getLATok(2) != TK__ALIGNAS)))
    {
        getTok();
        const auto previousSize = parts.size();
        name = DeclaratorParts(parts, mode);
        if (name == NULL_AST_NODE && parts.size() == previousSize)
            parsingError("expected an abstract declarator inside parentheses");
        check(TK_RPAR);
    }
    else if (mode == DeclaratorName::Required)
        parsingError("expected identifier or parenthesized declarator");

    while (_sym == TK_LBRACK || _sym == TK_LPAR)
    {
        if (parts.size() == 128)
            parsingError("derived declarator nesting limit exceeded");
        const auto &token = getLookahead();
        DeclaratorPart part{_sym == TK_LBRACK ? DeclaratorPart::Array :
                                              DeclaratorPart::Function,
                            token.line, token.col};
        getTok();
        if (part.kind == DeclaratorPart::Array)
        {
            if (_sym == TK_STATIC || isTypeQualifier(_sym))
                parsingError("array parameter qualifiers and static bounds are unsupported");
            if (_sym == TK_TIMES && getLATok(2) == TK_RBRACK)
                parsingError("variable-length array star bounds are unsupported");
            if (_sym != TK_RBRACK)
                part.operand = AssignmentExpression();
            check(TK_RBRACK);
        }
        else
        {
            if (_sym == TK_IDENT && !isTypeSpecifier(_sym, 1))
                parsingError("old-style function identifier lists are unsupported");
            _stb.openScope();
            part.scope = _stb.getTopScope();
            if (_sym != TK_RPAR)
                part.operand = ParameterTypeList(part.variadic);
            check(TK_RPAR);
            _stb.closeScope();
        }
        parts.push_back(part);
    }
    if (pointers.size() > 128 - parts.size())
        parsingError("derived declarator nesting limit exceeded");
    parts.insert(parts.end(), pointers.rbegin(), pointers.rend());
    return name;
}

// gccDeclaratorExtension
//     :   '__asm' '(' StringLiteral+ ')'
//     |   gccAttributeSpecifier
//     ;
ASTNode *CParser::GccDeclaratorExtension()
{
    if (_sym == TK___ASM)
    {
        getTok();
        check(TK_LPAR);
        check(TK_STRING_LIT);
        while (_sym == TK_STRING_LIT)
        {
            getTok();
        }
        check(TK_RPAR);
    }
    else if (_sym == TK___ATTRIBUTE__)
    {
        GccAttributeSpecifier();
    }
    else
    {
        parsingError("expected '__asm' or '__attribute__'");
    }

    return NULL_AST_NODE;
}

// gccAttributeSpecifier
//     :   '__attribute__' '(' '(' gccAttributeList ')' ')'
//     ;
ASTNode *CParser::GccAttributeSpecifier()
{
    check(TK___ATTRIBUTE__);
    check(TK_LPAR);
    check(TK_LPAR);
    GccAttributeList();
    check(TK_RPAR);
    check(TK_RPAR);

    return NULL_AST_NODE;
}

// gccAttributeList
//     :   gccAttribute (',' gccAttribute)*
//     |   // empty
//     ;
ASTNode *CParser::GccAttributeList()
{
    if (_sym == TK_IDENT)
    {
        SequenceASTNode *lst = new SequenceASTNode(GccAttribute());

        while (_sym == TK_COMMA)
        {
            getTok();
            lst->add(GccAttribute());
        }
        return lst;
    }
    else
    {
        return NULL_AST_NODE;
    }
}

// gccAttribute
//     :   ~(',' | '(' | ')') // relaxed def for "identifier or reserved word"
//         ('(' argumentExpressionList? ')')?
//     |   // empty
//     ;
ASTNode *CParser::GccAttribute()
{
    if (_sym == TK_IDENT)
    {
        getTok();
        if (_sym == TK_LPAR)
        {
            getTok();
            if (_sym != TK_RPAR)
            {
                ArgumentExpressionList();
            }
            check(TK_RPAR);
        }
    }

    return NULL_AST_NODE;
}

ASTNode *CParser::ParameterTypeList(bool &variadic)
{
    auto *paramList = static_cast<SequenceASTNode *>(ParameterList());
    variadic = false;
    if (_sym == TK_COMMA)
    {
        getTok();
        check(TK_ELLIPSIS);
        variadic = true;
    }
    const auto &parameters = paramList->getElements();
    for (auto *node : parameters)
    {
        auto *param = static_cast<ParmDeclASTNode *>(node);
        auto *type = param->getType();
        if (type->getKind() == NK_VOID_TYPE &&
            (parameters.size() != 1 || variadic || param->getName() != NULL_AST_NODE ||
             (param->getFlags() & DS_STORAGE_MASK) || (type->getFlags() & DS_QUALIFIER_MASK)))
            _ast->error(node->getLineNum(),
                        "void must be the sole unnamed, unqualified function parameter");
    }
    return paramList;
}

ASTNode *CParser::ParameterList()
{
    SequenceASTNode *prms = new SequenceASTNode(ParameterDeclaration());

    while (_sym == TK_COMMA)
    {
        if (getLATok(2) == TK_ELLIPSIS)
            break;

        getTok();
        prms->add(ParameterDeclaration());
    }
    return prms;
}

// parameter_declaration
//     : declaration_specifiers declarator
//     | declaration_specifiers abstract_declarator
//     | declaration_specifiers
//     ;
ASTNode *CParser::ParameterDeclaration()
{
    const int line = getLookahead().line;
    ASTNode *typeSpec = DeclarationSpecifiers();
    const unsigned flags = typeSpec->getFlags();
    if (flags & (DS_STORAGE_MASK & ~SCS_REGISTER))
        _ast->error(line, "register is the only storage class allowed on a parameter");
    if (flags & (FS_INLINE | FS_NORETURN))
        _ast->error(line, "function specifier is not allowed on a parameter");
    auto *name = Declarator(typeSpec, DeclaratorName::Optional);
    auto *parameter = new ParmDeclASTNode(typeSpec, name);
    parameter->setLineNum(line);
    parameter->setFlags(flags & DS_STORAGE_MASK);
    // The name's scope starts after this declarator, not after the whole list.
    parameter->declare(_ast);
    return parameter;
}

ASTNode *CParser::IdentifierList()
{
    SequenceASTNode *res;

    check(TK_IDENT);
    res = new SequenceASTNode(new IdentASTNode(_tok->sval, _tok->line));

    while (_sym == TK_COMMA)
    {
        getTok();
        check(TK_IDENT);
        res->add(new IdentASTNode(_tok->sval, _tok->line));
    }

    return res;
}

// static bool isFirstOfTypeName(unsigned n)
// {
//     return false;
// }

// type_name
//     : specifier_qualifier_list abstract_declarator
//     | specifier_qualifier_list
//     ;
ASTNode *CParser::TypeName()
{
    ASTNode *type = SpecifierQualifierList();
    Declarator(type, DeclaratorName::Forbidden);
    return type;
}

ASTNode *CParser::Initializer()
{
    ParseDepth depth(*this);
    ASTNode *res = NULL_AST_NODE;

    if (_sym == TK_LBRACE)
    {
        getTok();

        res = InitializerList();

        check(TK_RBRACE);
    }
    else
    {
        res = AssignmentExpression();
    }

    return res;
}

ASTNode *CParser::InitializerList()
{
    SequenceASTNode *res;
    ASTNode *init;

    const auto parseInitializer = [&]() -> ASTNode * {
        if (_sym == TK_PERIOD || _sym == TK_LBRACK)
            parsingError("designated initializers are unsupported");
        return Initializer();
    };
    init = parseInitializer();
    res = new SequenceASTNode();
    res->addElement(init);

    while (_sym == TK_COMMA)
    {
        getTok();
        if (_sym == TK_RBRACE)
            break;

        init = parseInitializer();
        res->addElement(init);
    }

    return res;
}

//
// Statements
//

ASTNode *CParser::Statement()
{
    ParseDepth depth(*this);
    ASTNode *stmt = NULL_AST_NODE;
    // ASTNode *expr = NULL_AST_NODE;

    if (_sym == TK_IDENT)
    {
        if (getLATok(2) == TK_COLON)
            stmt = LabeledStatement();
        else
            stmt = ExpressionStatement();
    }
    else if (isFirstOfPostfixExpression(_sym) || _sym == TK_TIMES ||
             _sym == TK_AND || _sym == TK_PLUS || _sym == TK_MINUS ||
             _sym == TK_TILDA || _sym == TK_NOT || _sym == TK_INC_OP ||
             _sym == TK_DEC_OP || _sym == TK_SIZEOF || _sym == TK__ALIGNOF ||
             _sym == TK_SEMICOLON)
    {
        // This is a function call of type: (*func)(x, y)
        stmt = ExpressionStatement();
    }
    else if (_sym == TK_CASE || _sym == TK_DEFAULT)
    {
        stmt = LabeledStatement();
    }
    else if (_sym == TK_LBRACE)
    {
        stmt = CompoundStatement();
    }
    else if (_sym == TK_IF || _sym == TK_SWITCH)
    {
        stmt = SelectionStatement();
    }
    else if (_sym == TK_WHILE || _sym == TK_DO || _sym == TK_FOR)
    {
        stmt = IterationStatement();
    }
    else if (_sym == TK_GOTO || _sym == TK_CONTINUE || _sym == TK_BREAK ||
             _sym == TK_RETURN)
    {
        stmt = JumpStatement();
    }
    else if (_sym == TK___ASM)
    {
        parsingError("inline assembly is unsupported in C source");
    }
    else
    {
        parsingError("expected statement");
    }

    return stmt;
}

ASTNode *CParser::LabeledStatement()
{
    const Token keyword = getLookahead();
    ASTNode *labstmt = NULL_AST_NODE;

    if (_sym == TK_IDENT)
    {
        if (!_labels.emplace(keyword.sval, keyword).second)
            _ast->errorAt(keyword.line, keyword.col, "duplicate label '%s'",
                          keyword.sval.c_str());
        getTok();
        ASTNode *label = new IdentASTNode(_tok->sval, _tok->line);
        check(TK_COLON);
        ASTNode *stmt = Statement();
        labstmt = new LabelStmtASTNode(label, stmt);
    }
    else if (_sym == TK_CASE)
    {
        if (_switchDefaults.empty())
            _ast->errorAt(keyword.line, keyword.col, "case label outside a switch");
        getTok();
        ASTNode *expr = ConstantExpression();
        check(TK_COLON);
        ASTNode *stmt = Statement();
        labstmt = new CaseLabelASTNode(expr, stmt);
    }
    else if (_sym == TK_DEFAULT)
    {
        if (_switchDefaults.empty())
            _ast->errorAt(keyword.line, keyword.col, "default label outside a switch");
        if (_switchDefaults.back())
            _ast->errorAt(keyword.line, keyword.col, "duplicate default label in switch");
        _switchDefaults.back() = true;
        getTok();
        check(TK_COLON);
        labstmt = new CaseLabelASTNode(NULL_AST_NODE, Statement());
    }

    labstmt->setLineNum(keyword.line);
    return labstmt;
}

ASTNode *CParser::ScopedStatement()
{
    if (_sym == TK_LBRACE)
        return CompoundStatement();
    _stb.openScope();
    auto *statement = Statement();
    _stb.closeScope();
    return statement;
}

ASTNode *CParser::LoopBody()
{
    ++_loopDepth;
    auto *statement = ScopedStatement();
    --_loopDepth;
    return statement;
}

ASTNode *CParser::CompoundStatement()
{
    check(TK_LBRACE);
    _stb.openScope();
    ASTNode *compstmt = StmtOrDeclList();
    static_cast<CompoundStmtASTNode *>(compstmt)->setScope(_stb.getTopScope());
    _stb.closeScope();
    check(TK_RBRACE);

    return compstmt;
}

ASTNode *CParser::FunctionBody()
{
    if (_sym != TK_LBRACE)
    { // Forward declaration
        return NULL_AST_NODE;
    }

    getTok(); // eat '{'
    ASTNode *compound = StmtOrDeclList();
    static_cast<CompoundStmtASTNode *>(compound)->setScope(_stb.getTopScope());
    check(TK_RBRACE);

    return compound;
}

ASTNode *CParser::DeclarationList()
{
    // Declaration()
    return NULL_AST_NODE;
}

ASTNode *CParser::StmtOrDeclList()
{
    ASTNode *stmts = NULL_AST_NODE;
    ASTNode *decls = NULL_AST_NODE;
    ASTNode *items = NULL_AST_NODE;
    const auto append = [](ASTNode *&list, ASTNode *item) {
        if (list == NULL_AST_NODE)
            list = new SequenceASTNode();
        auto *sequence = static_cast<SequenceASTNode *>(list);
        if (item->getKind() == NK_LIST)
            for (ASTNode *element :
                 static_cast<SequenceASTNode *>(item)->getElements())
                sequence->addElement(element);
        else
            sequence->addElement(item);
    };

    while (_sym != TK_RBRACE && _sym != TK_EOF)
    {
        const bool assertion = _sym == TK__STATIC_ASSERT;
        // A label has its own namespace, even if its name is also a typedef.
        const bool declaration =
            !(_sym == TK_IDENT && getLATok(2) == TK_COLON) && startsDeclaration();
        ASTNode *item = NULL_AST_NODE;
        if (declaration)
        {
            if (assertion)
                item = StaticAssertDeclaration();
            else
            {
                ASTNode *declSpec = DeclarationSpecifiers();
                if (declSpec->getFlags() & SCS_TYPEDEF)
                    item = TypedefDeclaration(declSpec);
                else
                    item = Declaration(declSpec);
            }

            append(decls, item);
        }
        else
        {
            item = Statement();
            append(stmts, item);
        }

        append(items, item);
    }

    if (items == NULL_AST_NODE)
        items = new SequenceASTNode();
    return new CompoundStmtASTNode(decls, stmts, items);
}

ASTNode *CParser::SelectionStatement()
{
    ASTNode *selstmt = NULL_AST_NODE;
    const int line = getLookahead().line;
    _stb.openScope();

    if (_sym == TK_IF)
    {
        ASTNode *expr;
        ASTNode *then_clause;
        ASTNode *else_clause = NULL_AST_NODE;

        getTok();
        check(TK_LPAR);
        expr = Expression();
        check(TK_RPAR);
        then_clause = ScopedStatement();

        if (_sym == TK_ELSE)
        {
            getTok();
            else_clause = ScopedStatement();
        }

        selstmt = new IfStmtASTNode(expr, then_clause, else_clause);
    }
    else if (_sym == TK_SWITCH)
    {
        ASTNode *expr;
        ASTNode *stmt;

        getTok();
        check(TK_LPAR);
        expr = Expression();
        check(TK_RPAR);
        _switchDefaults.push_back(false);
        stmt = ScopedStatement();
        _switchDefaults.pop_back();

        selstmt = new SwitchStmtASTNode(expr, stmt);
    }

    _stb.closeScope();
    selstmt->setLineNum(line);
    return selstmt;
}

ASTNode *CParser::IterationStatement()
{
    ASTNode *iterstmt = NULL_AST_NODE;
    ASTNode *expr = NULL_AST_NODE;
    ASTNode *stmt = NULL_AST_NODE;
    const int line = getLookahead().line;
    _stb.openScope();

    if (_sym == TK_WHILE)
    {
        getTok();
        check(TK_LPAR);

        // Get condition
        expr = Expression();
        check(TK_RPAR);

        // Get while body
        stmt = LoopBody();

        // Create while statement
        iterstmt = new WhileStmtASTNode(expr, stmt);
    }
    else if (_sym == TK_DO)
    {
        getTok();

        // Get do-while body
        stmt = LoopBody();
        check(TK_WHILE);
        check(TK_LPAR);

        // Get condition
        expr = Expression();
        check(TK_RPAR);
        check(TK_SEMICOLON);

        // Create while statement
        iterstmt = new DoStmtASTNode(expr, stmt);
    }
    else if (_sym == TK_FOR)
    {
        ASTNode *step = NULL_AST_NODE;
        ASTNode *init = NULL_AST_NODE;

        getTok();
        check(TK_LPAR);

        const bool declaration = startsDeclaration();
        if (declaration)
        {
            if (_sym == TK__STATIC_ASSERT)
                parsingError("expected an object declaration in the for initializer");
            ASTNode *declSpec = DeclarationSpecifiers();
            if (declSpec->getFlags() & (SCS_TYPEDEF | SCS_EXTERN | SCS_STATIC))
                _ast->error(line, "for initializer can only declare auto or register objects");
            init = Declaration(declSpec);
            if (init->getKind() == NK_LIST)
                for (auto *node : static_cast<SequenceASTNode *>(init)->getElements())
                    if (node->getKind() != NK_VAR_DECL)
                        _ast->error(line, "for initializer can only declare auto or register objects");
        }
        else if (_sym != TK_SEMICOLON)
            init = Expression();

        if (!declaration)
            check(TK_SEMICOLON);

        if (_sym != TK_SEMICOLON)
            expr = Expression();

        check(TK_SEMICOLON);

        if (_sym != TK_RPAR)
            step = Expression();

        check(TK_RPAR);

        stmt = LoopBody();

        // Create for statement
        iterstmt = new ForStmtASTNode(init, expr, step, stmt);
    }

    _stb.closeScope();
    iterstmt->setLineNum(line);
    return iterstmt;
}

ASTNode *CParser::JumpStatement()
{
    ASTNode *jumpstmt = NULL_AST_NODE;
    ASTNode *expr = NULL_AST_NODE;
    const int line = getLookahead().line, column = getLookahead().col;

    if (_sym == TK_GOTO)
    {
        getTok();
        check(TK_IDENT);
        _gotos.push_back(*_tok);
        jumpstmt =
            new GotoStmtASTNode(new IdentASTNode(_tok->sval, _tok->line));
        check(TK_SEMICOLON);
    }
    else if (_sym == TK_CONTINUE)
    {
        if (!_loopDepth)
            _ast->errorAt(line, column, "continue statement outside a loop");
        getTok();
        jumpstmt = new ContinueStmtASTNode();
        check(TK_SEMICOLON);
    }
    else if (_sym == TK_BREAK)
    {
        if (!_loopDepth && _switchDefaults.empty())
            _ast->errorAt(line, column, "break statement outside a loop or switch");
        getTok();
        jumpstmt = new BreakStmtASTNode();
        check(TK_SEMICOLON);
    }
    else if (_sym == TK_RETURN)
    {
        getTok();
        if (_sym != TK_SEMICOLON)
            expr = Expression();
        const bool returnsVoid = _returnType->getKind() == NK_VOID_TYPE;
        if (returnsVoid && expr != NULL_AST_NODE)
            _ast->errorAt(line, column, "void function cannot return a value");
        if (!returnsVoid && expr == NULL_AST_NODE)
            _ast->errorAt(line, column, "non-void function must return a value");
        check(TK_SEMICOLON);
        jumpstmt = new ReturnStmtASTNode(_returnType, expr);
    }

    jumpstmt->setLineNum(line);
    return jumpstmt;
}

ASTNode *CParser::ExpressionStatement()
{
    ASTNode *expr = NULL_AST_NODE;

    if (_sym != TK_SEMICOLON)
        expr = Expression();
    else
        expr = new NopExprASTNode();
    check(TK_SEMICOLON);

    return expr;
}

//
// Translation unit
//

ASTNode *CParser::TranslationUnit()
{
    ASTNode *extdecl = NULL_AST_NODE;
    ASTNode *tunit = NULL_AST_NODE;

    _stb.openScope();

    for (;;)
    {
        if (startsDeclaration() || _sym == TK___ASM || _sym == TK___ATTRIBUTE__)
        {
            extdecl = ExternalDeclaration();
            if (tunit == NULL_AST_NODE)
                tunit = new SequenceASTNode(extdecl);
            else
                static_cast<SequenceASTNode *>(tunit)->add(extdecl);
        }
        else if (_sym == TK_EOF)
        {
            break;
        }
        else
        {
            parsingError("expected external declaration");
        }
    }

    if (tunit == NULL_AST_NODE)
        tunit = new SequenceASTNode();
    static_cast<SequenceASTNode *>(tunit)->setScope(_stb.getTopScope());
    _stb.closeScope();

    return tunit;
}

ASTNode *CParser::ExternalDeclaration()
{
    if (_sym == TK__STATIC_ASSERT)
        return StaticAssertDeclaration();
    ASTNode *declSpec = DeclarationSpecifiers();
    const unsigned flags = declSpec->getFlags();
    if (flags & (SCS_AUTO | SCS_REGISTER))
        parsingError("auto and register are not allowed at file scope");
    if (flags & SCS_TYPEDEF)
        return TypedefDeclaration(declSpec);
    if (_sym == TK_SEMICOLON)
        return Declaration(declSpec);

    ASTNode *type = declSpec;
    STScope *parameterScope = nullptr;
    ASTNode *name = Declarator(type, DeclaratorName::Required, &parameterScope);
    ASTNode *first = InitDeclaratorAfterName(declSpec, type, name, parameterScope, true);
    if (first->getKind() == NK_FUNCTION_DECL &&
        !static_cast<FunctionDeclASTNode *>(first)->isForwardDeclaration())
        return first;

    auto *list = new SequenceASTNode(first);
    while (_sym == TK_COMMA)
    {
        getTok();
        list->add(InitDeclarator(declSpec));
    }
    check(TK_SEMICOLON);
    return list;
}

ASTNode *CParser::FunctionDeclaration(FunctionTypeASTNode *type, ASTNode *name,
                                     unsigned flags, STScope *parameterScope,
                                     bool allowDefinition)
{
    if ((flags & (SCS_AUTO | SCS_REGISTER)) ||
        (_stb.getLevel() != 0 && (flags & SCS_STATIC)))
        parsingError("invalid storage class on a function declaration");
    if (_sym == TK_ASSIGN)
        parsingError("a function declaration cannot have an initializer");

    const char *spelling = AST_IDENT_VALUE(name);
    auto *existing = _stb.findLocal(spelling);
    if (existing == _stb.noObj)
        _stb.insert(spelling, STOK_FUNC, _stb.noType)->astType = type;
    else if (existing->kind != STOK_FUNC)
        _ast->error(name->getLineNum(), "name '%s' is already declared", spelling);

    auto *funcDecl = new FunctionDeclASTNode(type, name, type->getPrms(), NULL_AST_NODE);
    funcDecl->setFlags(flags);
    funcDecl->setLineNum(name->getLineNum());
    funcDecl->setPrototype(type->hasPrototype());
    funcDecl->setVariadic(type->isVariadic());
    funcDecl->setScope(parameterScope);

    if (_sym == TK_LBRACE)
    {
        if (!allowDefinition)
            parsingError("function definition is not allowed here");
        if (!parameterScope)
            parsingError("function definition requires an explicit function declarator");
        if ((flags & FS_INLINE) && !(flags & SCS_STATIC))
            parsingError("inline functions with external linkage are unsupported");
        if (type->hasPrototype())
            for (auto *node : static_cast<SequenceASTNode *>(type->getPrms())->getElements())
            {
                auto *parameter = static_cast<ParmDeclASTNode *>(node);
                if (parameter->getName() == NULL_AST_NODE &&
                    parameter->getType()->getKind() != NK_VOID_TYPE)
                    _ast->error(parameter->getLineNum(),
                                "function definition requires named parameters");
            }
        _stb.reopenScope(parameterScope);
        _returnType = type->getType();
        funcDecl->setBody(FunctionBody());
        _returnType = nullptr;
        for (const auto &target : _gotos)
            if (!_labels.count(target.sval))
                _ast->errorAt(target.line, target.col, "goto to undefined label '%s'",
                              target.sval.c_str());
        _gotos.clear();
        _labels.clear();
        _stb.closeScope();
    }

    return funcDecl;
}

} // namespace cparser
