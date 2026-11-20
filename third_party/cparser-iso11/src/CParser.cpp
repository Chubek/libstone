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
        if (object != _stb.noObj && object->kind == STOK_CON)
        {
            res = new IntegerConstASTNode(
                static_cast<std::uint64_t>(object->ival), _tok->sval);
            res->setLineNum(_tok->line);
        }
        else
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
           n == TK__GENERIC;
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
    ASTNode *initlist = NULL_AST_NODE;

    if (_sym != TK_SEMICOLON)
        initlist = InitDeclaratorList(declSpec);

    check(TK_SEMICOLON);

    if (initlist != NULL_AST_NODE)
    {
        return initlist;
    }
    else
    {
        // Here is a forward struct declaration so far.
        return declSpec;
    }
}

ASTNode *CParser::TypedefDeclaration(ASTNode *declSpec)
{
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
    _inTypedef = false;
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

ASTNode *CParser::DeclarationSpecifiers(bool typeOnly)
{
    ParseDepth depth(*this);
    ASTNode *declSpec = NULL_AST_NODE;
    unsigned flags = 0x0;
    unsigned baseQualifiers = 0;
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
                    baseQualifiers = declSpec->getFlags() & DS_QUALIFIER_MASK;
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
    declSpec->setFlags(flags | baseQualifiers);
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
                                           enumeration->getBody());
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
    ASTNode *declaratorType = cloneType(typeSpec);
    ASTNode *name = Declarator(declaratorType);
    return InitDeclaratorAfterName(typeSpec, declaratorType, name);
}

ASTNode *CParser::InitDeclaratorAfterName(ASTNode *typeSpec, ASTNode *type,
                                         ASTNode *name)
{
    ASTNode *init = NULL_AST_NODE;
    if (typeSpec->getFlags() & (FS_INLINE | FS_NORETURN))
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
        _inTypedef = true;
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
        const std::string key =
            std::string(typeKind == NK_STRUCT_TYPE ? "struct:" : "union:") + _tok->sval;
        const std::string otherKey =
            std::string(typeKind == NK_STRUCT_TYPE ? "union:" : "struct:") + _tok->sval;
        const std::string enumKey = "enum:" + _tok->sval;
        const bool introduces = _sym == TK_LBRACE || _sym == TK_SEMICOLON;
        const auto lookup = [&](const std::string &name) {
            return introduces ? _stb.findLocal(name.c_str()) : _stb.find(name.c_str());
        };
        auto *found = lookup(key);
        for (const auto *other : {lookup(otherKey), lookup(enumKey)})
            if (other != _stb.noObj && (found == _stb.noObj || other->level >= found->level))
                parsingError("tag refers to a different kind of type");
        if (found == _stb.noObj)
        {
            recordType = _stb.allocType(recordKind);
            recordType->tagName = _tok->sval;
            _stb.insert(key.c_str(), STOK_TYPE, recordType);
        }
        else
            recordType = found->type;
    }
    else if (_sym == TK_LBRACE)
        recordType = _stb.allocType(recordKind);
    else
        parsingError("expected a tag or record definition");

    if (_sym == TK_LBRACE)
    {
        if (recordType->fields)
            parsingError("record tag is already defined");
        getTok();
        _stb.openScope();
        typeBody = StructDeclarationList();
        if (typeKind == NK_UNION_TYPE)
        {
            unsigned maxFieldTypeSize = 0;
            for (auto *field = _stb.getTopScope()->locals; field; field = field->next)
                if (maxFieldTypeSize < field->type->size)
                    maxFieldTypeSize = field->type->size;
            _stb.getTopScope()->size = maxFieldTypeSize;
        }
        recordType->fields = _stb.getTopScope()->locals;
        recordType->size = _stb.getTopScope()->size;
        _stb.closeScope();
        check(TK_RBRACE);
    }
    if (typeKind == NK_STRUCT_TYPE)
        return new StructTypeASTNode(typeName, typeBody, recordType);
    return new UnionTypeASTNode(typeName, typeBody, recordType);
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
    //ASTNode *typeSpec = TypeSpecifier();
    ASTNode *typeSpec = SpecifierQualifierList();
    SequenceASTNode *declaratorList = StructDeclaratorList(typeSpec);

    declaratorList->declare(_ast);
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

    if (_sym == TK_IDENT)
    {
        STType *enumType;

        getTok();
        name = new IdentASTNode(_tok->sval);
        enumType = _stb.allocType(STTK_ENUM);
        enumType->tagName = _tok->sval;
        const std::string key = std::string("enum:") + _tok->sval;
        _stb.insert(key.c_str(), STOK_TYPE, enumType);
    }

    if (_sym == TK_LBRACE)
    {
        getTok();
        body = EnumeratorList();
        check(TK_RBRACE);
    }

    return new EnumeralTypeASTNode(name, body);
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

// alignment_specifier
//     : ALIGNAS '(' type_name ')'
//     | ALIGNAS '(' constant_expression ')'
//     ;
ASTNode *CParser::AlignmentSpecifier()
{
    check(TK__ALIGNAS);
    check(TK_LPAR);
    // TODO: This is incorrect, use isFirstOfTypeName() instead.
    if (isTypeSpecifier(_sym, 1))
    {
        TypeName();
    }
    else
    {
        ConstantExpression();
    }
    check(TK_RPAR);

    return NULL_AST_NODE;
}

ASTNode *CParser::Declarator(ASTNode * &typeSpec)
{
    ParseDepth depth(*this);
    unsigned pointers = 0;
    while (_sym == TK_TIMES)
    {
        if (++pointers > 128)
            parsingError("pointer declarator nesting limit exceeded");
        const unsigned storage = typeSpec->getFlags() & DS_STORAGE_MASK;
        getTok();
        typeSpec = new PointerTypeASTNode(typeSpec);
        unsigned qualifiers = 0;
        while (isTypeQualifier(_sym))
            TypeQualifier(qualifiers);
        typeSpec->setFlags(storage | qualifiers);
    }

    ASTNode *decl = DirectDeclarator(typeSpec);

    if (_sym == TK___ASM || _sym == TK___ATTRIBUTE__)
        parsingError("GCC declarator extensions are unsupported");

    return decl;
}

ASTNode *CParser::DirectDeclarator(ASTNode * &typeSpec)
{
    ASTNode *declr = NULL_AST_NODE;
    FunctionTypeASTNode *funcType = nullptr;
    bool isFuncPtr = false;

    if (_sym == TK_IDENT)
    {
        getTok();
        declr = new IdentASTNode(_tok->sval, _tok->line);
    }
    else
    {
        check(TK_LPAR);
        if (_sym == TK_TIMES)
        {
            getTok();
            if (_sym == TK_TIMES)
                parsingError("nested parenthesized pointer declarators are unsupported");

            unsigned flags = 0;
            // TODO: Check the grammar
            if (isTypeQualifier(_sym))
            {
                TypeQualifier(flags);
            }

            // This is a function pointer.
            isFuncPtr = true;
            // FIXME: Make this direct type without T_POINTER.
            funcType = new FunctionTypeASTNode(typeSpec, NULL_AST_NODE);
            typeSpec = new PointerTypeASTNode(funcType);
        }
        // Only a bare parenthesized name or (*name)(parameters) is modeled.
        // An array or another pointer inside the parentheses changes binding.
        check(TK_IDENT);
        declr = new IdentASTNode(_tok->sval, _tok->line);
        check(TK_RPAR);
    }

    if (isFuncPtr)
    {
        if (_sym != TK_LPAR)
            parsingError("parenthesized non-function pointer declarator is unsupported");
        getTok();
        if (_sym != TK_RPAR)
        {
            _stb.openScope();
            ASTNode *funcPrms = ParameterTypeList();
            _stb.closeScope();
            funcType->setPrms(funcPrms);
            funcType->setPrototype(true);
            funcType->setVariadic(_lastParameterListVariadic);
        }
        check(TK_RPAR);
        if (_sym == TK_LBRACK || _sym == TK_LPAR)
            parsingError("nested function-pointer declarator is unsupported");
        return declr;
    }

    std::vector<ASTNode *> dimensions;
    while (_sym == TK_LBRACK)
    {
        if (dimensions.size() == 128)
            parsingError("array declarator nesting limit exceeded");
        getTok();
        ASTNode *dimension =
            _sym == TK_RBRACK ? NULL_AST_NODE : AssignmentExpression();
        dimensions.push_back(dimension);
        check(TK_RBRACK);
    }
    for (auto i = dimensions.rbegin(); i != dimensions.rend(); ++i)
        typeSpec = new ArrayTypeASTNode(typeSpec, *i);

    return declr;
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

ASTNode *CParser::ParameterTypeList()
{
    ASTNode *paramList = ParameterList();
    _lastParameterListVariadic = false;
    if (_sym == TK_COMMA)
    {
        getTok();
        check(TK_ELLIPSIS);
        _lastParameterListVariadic = true;
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

static bool isFirstOfAbstractDeclarator(unsigned n)
{
    return n == TK_TIMES || n == TK_LPAR || n == TK_LBRACK;
}

// parameter_declaration
//     : declaration_specifiers declarator
//     | declaration_specifiers abstract_declarator
//     | declaration_specifiers
//     ;
ASTNode *CParser::ParameterDeclaration()
{
    ASTNode *typeSpec = DeclarationSpecifiers();
    unsigned pointers = 0;
    while (_sym == TK_TIMES)
    {
        if (++pointers > 128)
            parsingError("pointer parameter nesting limit exceeded");
        getTok();
        typeSpec = new PointerTypeASTNode(typeSpec);
        unsigned qualifiers = 0;
        while (isTypeQualifier(_sym))
            TypeQualifier(qualifiers);
        typeSpec->setFlags(qualifiers);
    }
    if (_sym == TK_IDENT)
    {
        ASTNode *d = Declarator(typeSpec);
        return new ParmDeclASTNode(typeSpec, d);
    }
    if (_sym == TK_LPAR)
        parsingError("parenthesized declarator parameters are unsupported");

    std::vector<ASTNode *> dimensions;
    while (_sym == TK_LBRACK)
    {
        if (dimensions.size() == 128)
            parsingError("array parameter nesting limit exceeded");
        getTok();
        dimensions.push_back(_sym == TK_RBRACK ? NULL_AST_NODE :
                                                AssignmentExpression());
        check(TK_RBRACK);
    }
    for (auto i = dimensions.rbegin(); i != dimensions.rend(); ++i)
        typeSpec = new ArrayTypeASTNode(typeSpec, *i);
    return new ParmDeclASTNode(typeSpec, NULL_AST_NODE);
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
    unsigned pointers = 0;
    while (_sym == TK_TIMES)
    {
        if (++pointers > 128)
            parsingError("pointer type nesting limit exceeded");
        getTok();
        type = new PointerTypeASTNode(type);
        unsigned qualifiers = 0;
        while (isTypeQualifier(_sym))
            TypeQualifier(qualifiers);
        type->setFlags(type->getFlags() | qualifiers);
    }
    return type;
}

ASTNode *CParser::AbstractDeclarator()
{
    if (_sym == TK_TIMES)
    {
        getTok();
        if (_sym == TK_LPAR || _sym == TK_LBRACK)
            DirectAbstractDeclarator();
    }
    else if (_sym == TK_LPAR || _sym == TK_LBRACK)
    {
        DirectAbstractDeclarator();
    }
    else
    {
        parsingError("expected '*' or direct abstract declarator");
    }

    return NULL_AST_NODE;
}

ASTNode *CParser::DirectAbstractDeclarator()
{
    if (_sym == TK_LPAR)
    {
        getTok();
        if (isFirstOfAbstractDeclarator(_sym))
            AbstractDeclarator();
        check(TK_RPAR);
    }
    else if (_sym == TK_LBRACK)
    {
        getTok();
        check(TK_RBRACK);
    }

    return NULL_AST_NODE;
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
    else if (_sym == TK_ASM)
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
    ASTNode *labstmt = NULL_AST_NODE;

    if (_sym == TK_IDENT)
    {
        getTok();
        ASTNode *label = new IdentASTNode(_tok->sval, _tok->line);
        check(TK_COLON);
        ASTNode *stmt = Statement();
        labstmt = new LabelStmtASTNode(label, stmt);
    }
    else if (_sym == TK_CASE)
    {
        getTok();
        ASTNode *expr = ConstantExpression();
        check(TK_COLON);
        ASTNode *stmt = Statement();
        labstmt = new CaseLabelASTNode(expr, stmt);
    }
    else if (_sym == TK_DEFAULT)
    {
        getTok();
        check(TK_COLON);
        labstmt = new CaseLabelASTNode(NULL_AST_NODE, Statement());
    }

    return labstmt;
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
        const bool declaration = assertion ||
                                 isStorageClassSpecifier(_sym) ||
                                 isTypeQualifier(_sym) ||
                                 isTypeSpecifier(_sym, 1) ||
                                 _sym == TK_INLINE || _sym == TK__NORETURN;
        ASTNode *item = NULL_AST_NODE;
        if (declaration)
        {
            if (assertion)
                item = StaticAssertDeclaration();
            else
            {
                ASTNode *declSpec = DeclarationSpecifiers();
                if (_inTypedef)
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

    if (_sym == TK_IF)
    {
        ASTNode *expr;
        ASTNode *then_clause;
        ASTNode *else_clause = NULL_AST_NODE;

        getTok();
        check(TK_LPAR);
        expr = Expression();
        check(TK_RPAR);
        then_clause = Statement();

        if (_sym == TK_ELSE)
        {
            getTok();
            else_clause = Statement();
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
        stmt = Statement();

        selstmt = new SwitchStmtASTNode(expr, stmt);
    }

    return selstmt;
}

ASTNode *CParser::IterationStatement()
{
    ASTNode *iterstmt = NULL_AST_NODE;
    ASTNode *expr = NULL_AST_NODE;
    ASTNode *stmt = NULL_AST_NODE;

    if (_sym == TK_WHILE)
    {
        getTok();
        check(TK_LPAR);

        // Get condition
        expr = Expression();
        check(TK_RPAR);

        // Get while body
        stmt = Statement();

        // Create while statement
        iterstmt = new WhileStmtASTNode(expr, stmt);
    }
    else if (_sym == TK_DO)
    {
        getTok();

        // Get do-while body
        stmt = Statement();
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
        _stb.openScope();

        const bool declaration = isStorageClassSpecifier(_sym) ||
                                 isTypeQualifier(_sym) ||
                                 isTypeSpecifier(_sym, 1);
        if (declaration)
        {
            ASTNode *declSpec = DeclarationSpecifiers();
            init = _inTypedef ? TypedefDeclaration(declSpec) :
                                Declaration(declSpec);
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

        stmt = Statement();
        _stb.closeScope();

        // Create for statement
        iterstmt = new ForStmtASTNode(init, expr, step, stmt);
    }

    return iterstmt;
}

ASTNode *CParser::JumpStatement()
{
    ASTNode *jumpstmt = NULL_AST_NODE;
    ASTNode *expr = NULL_AST_NODE;

    if (_sym == TK_GOTO)
    {
        getTok();
        check(TK_IDENT);
        jumpstmt =
            new GotoStmtASTNode(new IdentASTNode(_tok->sval, _tok->line));
        check(TK_SEMICOLON);
    }
    else if (_sym == TK_CONTINUE)
    {
        getTok();
        jumpstmt = new ContinueStmtASTNode();
        check(TK_SEMICOLON);
    }
    else if (_sym == TK_BREAK)
    {
        getTok();
        jumpstmt = new BreakStmtASTNode();
        check(TK_SEMICOLON);
    }
    else if (_sym == TK_RETURN)
    {
        getTok();
        if (_sym != TK_SEMICOLON)
            expr = Expression();
        check(TK_SEMICOLON);
        jumpstmt = new ReturnStmtASTNode(_ast->integerTypeASTNode, expr);
        jumpstmt->setLineNum(_tok->line);
    }

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
        if (isStorageClassSpecifier(_sym) || isTypeQualifier(_sym) ||
            isTypeSpecifier(_sym, 1) ||
            _sym == TK___ASM || _sym == TK___ATTRIBUTE__ ||
            _sym == TK_INLINE || _sym == TK__NORETURN ||
            _sym == TK__STATIC_ASSERT)
        {
            // _inTypedef is set in ExternalDeclaration/DeclartionSpecifiers/
            // StorageClassSpecifier
            extdecl = ExternalDeclaration();
            if (tunit == NULL_AST_NODE)
                tunit = new SequenceASTNode(extdecl);
            else
                static_cast<SequenceASTNode *>(tunit)->add(extdecl);

            //_inTypedef = false;
        }
        else if (_sym == TK_EOF)
        {
            break;
        }
        else
        {
            char msg[1024];
            sprintf(msg, "expected external declaration, found %d", _sym);
            parsingError(msg);
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
    // _inTypedef is set in DeclartionSpecifiers/StorageClassSpecifier
    ASTNode *declSpec = DeclarationSpecifiers();

    if (_inTypedef)
    {
        _inTypedef = false;
        ASTNode *decl = TypedefDeclaration(declSpec);
        return decl;
    }
    if (_sym == TK_IDENT && getLATok(2) == TK_LPAR)
    {
        return FunctionDefinition(declSpec);
    }
    if (_sym == TK_TIMES)
    {
        // Distinguish a function returning a pointer during declarator
        // parsing rather than guessing from a fixed number of tokens.
        ASTNode *type = cloneType(declSpec);
        ASTNode *name = Declarator(type);
        if (_sym == TK_LPAR)
        {
            return FunctionDefinition(type, name, declSpec->getFlags());
        }
        auto *list = new SequenceASTNode(
            InitDeclaratorAfterName(declSpec, type, name));
        while (_sym == TK_COMMA)
        {
            getTok();
            list->add(InitDeclarator(declSpec));
        }
        check(TK_SEMICOLON);
        return list;
    }
    return Declaration(declSpec);
}

ASTNode *CParser::FunctionDefinition(ASTNode *funcType)
{
    ASTNode *dummy = NULL_AST_NODE;
    ASTNode *funcName = Declarator(dummy);
    return FunctionDefinition(funcType, funcName, funcType->getFlags());
}

ASTNode *CParser::FunctionDefinition(ASTNode *funcType, ASTNode *funcName,
                                    unsigned flags)
{
    ASTNode *funcPrms = NULL_AST_NODE;

    check(TK_LPAR);

    FunctionDeclASTNode *funcDecl = new FunctionDeclASTNode(funcType,
                                                            funcName,
                                                            NULL_AST_NODE,
                                                             NULL_AST_NODE);
    funcDecl->setFlags(flags);

    // A tag introduced by a prototype is private to its parameter list.
    // For a definition the same scope continues into the function body.
    _stb.openScope();
    // TODO: Move ParameterTypeList to DirectDeclarator
    if (_sym != TK_RPAR)
    {
        funcPrms = ParameterTypeList();
        funcDecl->setPrms(funcPrms);
        funcDecl->setPrototype(true);
        funcDecl->setVariadic(_lastParameterListVariadic);
    }
    check(TK_RPAR);

    auto *fullType = new FunctionTypeASTNode(funcType, funcPrms);
    fullType->setFlags(flags & (DS_STORAGE_MASK | FS_INLINE | FS_NORETURN));
    fullType->setPrototype(funcDecl->hasPrototype());
    fullType->setVariadic(funcDecl->isVariadic());
    funcDecl->setType(fullType);

    if (_sym == TK___ASM || _sym == TK___ATTRIBUTE__)
        parsingError("GCC declarator extensions are unsupported");

    //_ast->declare(funcDecl);

    if (funcPrms != NULL_AST_NODE)
        funcPrms->declare(_ast);

    // Parameters are going inside of the function body.
    // if (funcPrms != NULL_AST_NODE)
    //    _ast->declare(funcPrms);

    if (_sym == TK_LBRACE)
    {
        if ((flags & FS_INLINE) && !(flags & SCS_STATIC))
            parsingError("inline functions with external linkage are unsupported");
        funcDecl->setBody(FunctionBody());
    }
    else
        check(TK_SEMICOLON);

    funcDecl->setScope(_stb.getTopScope());
    _stb.closeScope();
    _ast->setCurrentFuncDecl(nullptr);

    return funcDecl;
}

} // namespace cparser
