// Abstract syntax tree - header file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#ifndef ABSTRACT_SYNTAX_TREE_H
#define ABSTRACT_SYNTAX_TREE_H

#include <cstring>
#include <queue>
#include <stack>
#include <utility>

#include "ASTNode.h"

// Declaration flags
#define SCS_TYPEDEF 0x1
#define SCS_EXTERN 0x2
#define SCS_STATIC 0x4
#define SCS_AUTO 0x8
#define SCS_REGISTER 0x10

#define TQ_CONST 0x20
#define TQ_VOLATILE 0x40
#define TQ_RESTRICT 0x80
#define FS_INLINE 0x100
#define FS_NORETURN 0x200

#define DS_STORAGE_MASK (SCS_TYPEDEF | SCS_EXTERN | SCS_STATIC | SCS_AUTO | SCS_REGISTER)
#define DS_QUALIFIER_MASK (TQ_CONST | TQ_VOLATILE | TQ_RESTRICT)

#define UNLESS(x) if (!(x))
#define TYPE(x) x->type

// Match macros

#define AST_MATCH_NULL(n) (n == NULL_AST_NODE)

#define AST_MATCH_IDENT(n) (n->getKind() == NK_IDENT_NODE)

#define AST_MATCH_LIST(n) (n->getKind() == NK_LIST)

#define AST_MATCH_INTEGER_CONST(n) (n->getKind() == NK_INTEGER_CONST)

#define AST_MATCH_REAL_CONST(n) (n->getKind() == NK_REAL_CONST)

#define AST_MATCH_COMPLEX_CONST(n) (n->getKind() == NK_COMPLEX_CONST)

#define AST_MATCH_STRING_CONST(n) (n->getKind() == NK_STRING_CONST)

#define AST_MATCH_CHAR_CONST(n) (n->getKind() == NK_CHAR_CONST)

#define AST_MATCH_STRUCT_REF(n) (n->getKind() == NK_STRUCT_REF)

#define AST_MATCH_INDIRECT_REF(n) (n->getKind() == NK_INDIRECT_REF)

#define AST_MATCH_ARRAY_REF(n) (n->getKind() == NK_ARRAY_REF)

#define AST_MATCH_STRUCT_TYPE(n) (n->getKind() == NK_STRUCT_TYPE)

#define AST_MATCH_VAR_DECL(n) (n->getKind() == NK_VAR_DECL)

#define AST_MATCH_NOP_EXPR(n) (n->getKind() == NK_NOP_EXPR)

#define AST_COMPOUND_STMT_STMTS(n)                                             \
    static_cast<CompoundStmtASTNode *>(n)->getStmts()
#define AST_COMPOUND_STMT_DECLS(n)                                             \
    static_cast<CompoundStmtASTNode *>(n)->getDecls()
#define AST_IDENT_VALUE(n) static_cast<IdentASTNode *>(n)->getValue()
#define AST_IDENT_LINE_NUM(n) static_cast<IdentASTNode *>(n)->getLineNum()
#define AST_LIST(n) static_cast<ListASTNode *>(n)
#define AST_LIST_ELEMENTS(n) static_cast<ListASTNode *>(n)->getElements()
#define AST_INTEGER_CONST_VALUE(n)                                             \
    static_cast<IntegerConstASTNode *>(n)->getValue()
#define AST_INTEGER_CONST_LINE_NUM(n)                                          \
    static_cast<IntegerConstASTNode *>(n)->getLineNum()
#define AST_ARRAY_REF_EXPR(n) static_cast<ArrayRefASTNode *>(n)->getExpr()

namespace cparser
{

class ASTNode;

class AbstractSyntaxTree
{
    friend class ASTNode;

    ASTNode *_root;

    SymbolTable *_stb;

    STObject *_currentFuncDecl;

    int _errors;
    int _warnings;
    DiagnosticHandler _diagnostic;
    std::vector<ASTNode *> _nodes;

    void trackNode(ASTNode *node)
    {
        _nodes.push_back(node);
        node->owner = this;
        node->ownerIndex = _nodes.size() - 1;
        node->incRefCount();
    }

    void forgetNode(std::size_t index) noexcept { _nodes[index] = nullptr; }

public:
    STObject *currentRecordObj;

    ASTNode *voidTypeASTNode;
    ASTNode *integerTypeASTNode;
    ASTNode *longTypeASTNode;
    ASTNode *unsignedTypeASTNode;
    ASTNode *shortTypeASTNode;
    ASTNode *charTypeASTNode;
    ASTNode *boolTypeASTNode;
    ASTNode *floatTypeASTNode;

    std::vector<StructTypeASTNode *> structTypes;
    std::vector<UnionTypeASTNode *> unionTypes;

    STObject *getCurrentFuncDecl() { return _currentFuncDecl; }

    void setCurrentFuncDecl(STObject *currentFuncDecl)
    {
        _currentFuncDecl = currentFuncDecl;
    }

    [[noreturn]] void error(int lineNum, const char *format, ...);
    [[noreturn]] void errorAt(int line, int column, const char *format, ...);
    void warning(int lineNum, const char *format, ...);

    void setDiagnosticHandler(DiagnosticHandler handler)
    {
        _diagnostic = std::move(handler);
    }

    void setRoot(ASTNode *Root)
    {
        if (_root)
            DELETE_AST_NODE_REF(_root);
        ASSIGN_AST_NODE_REF(_root, Root);
    }
    ASTNode *getRoot() { return _root; }

    void declare();

    int getErrors() const { return _errors; }
    int getWarnings() const { return _warnings; }

    STType *getStbType(ASTNode *typeASTNode);

    void visit(TreeVisitor *visitor);

    SymbolTable *getSymbolTable() { return _stb; }

    StructTypeASTNode *createStructTypeASTNode(ASTNode *typeName,
                                               ASTNode *typeBody)
    {
        StructTypeASTNode *tmp = new StructTypeASTNode(typeName, typeBody);
        structTypes.push_back(tmp);
        return tmp;
    }

    AbstractSyntaxTree(SymbolTable *STB) : _stb(STB)
    {
        ASSIGN_AST_NODE_REF(voidTypeASTNode, new VoidTypeASTNode());
        // Cephyr's only native target is RV64 LP64, whose plain char is
        // unsigned. Rank is distinct from storage width.
        ASSIGN_AST_NODE_REF(charTypeASTNode, new IntegralTypeASTNode(1, false, 1, true));
        ASSIGN_AST_NODE_REF(boolTypeASTNode, new BooleanTypeASTNode());
        ASSIGN_AST_NODE_REF(shortTypeASTNode, new IntegralTypeASTNode(2, true, 2));
        ASSIGN_AST_NODE_REF(integerTypeASTNode,
                            new IntegralTypeASTNode(4, true, 3));
        ASSIGN_AST_NODE_REF(longTypeASTNode,
                            new IntegralTypeASTNode(8, true, 4));
        ASSIGN_AST_NODE_REF(unsignedTypeASTNode,
                            new IntegralTypeASTNode(4, false, 3));
        ASSIGN_AST_NODE_REF(floatTypeASTNode, new RealTypeASTNode(4, false));

        _currentFuncDecl = nullptr;
        currentRecordObj = nullptr;
        _errors = 0;
        _warnings = 0;

        _root = nullptr;
    }

    ~AbstractSyntaxTree()
    {
        if (_root)
            DELETE_AST_NODE_REF(_root);

        for (auto i = _nodes.rbegin(); i != _nodes.rend(); ++i)
        {
            ASTNode *node = *i;
            if (!node)
                continue;
            node->owner = nullptr;
            node->decRefCount();
            if (node->getRefCount() == 0)
                delete node;
        }

        DELETE_AST_NODE_REF(voidTypeASTNode);
        DELETE_AST_NODE_REF(charTypeASTNode);
        DELETE_AST_NODE_REF(boolTypeASTNode);
        DELETE_AST_NODE_REF(shortTypeASTNode);
        DELETE_AST_NODE_REF(integerTypeASTNode);
        DELETE_AST_NODE_REF(longTypeASTNode);
        DELETE_AST_NODE_REF(unsignedTypeASTNode);
        DELETE_AST_NODE_REF(floatTypeASTNode);

        for (unsigned n = 0; n < structTypes.size(); n++)
            delete structTypes[n];
    }
};

} // namespace cparser

#endif
