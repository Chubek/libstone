// Abstract syntax tree - implementation file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#include "../include/AbstractSyntaxTree.h"
#include "../include/ASTNode.h"
#include "../include/PrintTreeVisitor.h"
#include "../include/TreeVisitor.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <queue>
#include <vector>

namespace cparser
{

void AbstractSyntaxTree::error(int lineNum, const char *format, ...)
{
    char dest[4096];

    va_list argptr;
    va_start(argptr, format);
    vsnprintf(dest, sizeof(dest), format, argptr);
    va_end(argptr);
    ++_errors;
    failDiagnostic(_diagnostic, DiagnosticKind::SemanticError, lineNum, 0, dest);
}

void AbstractSyntaxTree::errorAt(int line, int column, const char *format, ...)
{
    char dest[4096];
    va_list argptr;
    va_start(argptr, format);
    vsnprintf(dest, sizeof(dest), format, argptr);
    va_end(argptr);
    ++_errors;
    failDiagnostic(_diagnostic, DiagnosticKind::SemanticError, line, column, dest);
}

void AbstractSyntaxTree::warning(int lineNum, const char *format, ...)
{
    char dest[4096];

    va_list argptr;
    va_start(argptr, format);
    vsnprintf(dest, sizeof(dest), format, argptr);
    va_end(argptr);
    ++_warnings;
    reportDiagnostic(_diagnostic, DiagnosticKind::Warning, lineNum, 0, dest);
}

void AbstractSyntaxTree::visit(TreeVisitor *visitor)
{
    if (_root->getKind() == NK_LIST)
        _stb->setTopScope(static_cast<SequenceASTNode *>(_root)->getScope());

    visitor->visit(static_cast<SequenceASTNode *>(_root));

    if (_root->getKind() == NK_LIST)
        _stb->setTopScope(nullptr);
}

STType *AbstractSyntaxTree::getStbType(ASTNode *typeASTNode)
{
    switch (typeASTNode->getKind())
    {
        case NK_VOID_TYPE:
            return _stb->voidType;
        case NK_INTEGRAL_TYPE:
        {
            IntegralTypeASTNode *intTypeASTNode =
                static_cast<IntegralTypeASTNode *>(typeASTNode);

            if (intTypeASTNode->getIsSigned())
            {
                switch (intTypeASTNode->getAlignment())
                {
                    case 1:
                        return _stb->charType;
                    case 2:
                        return _stb->shortType;
                    case 4:
                        return _stb->intType;
                    case 8:
                        return _stb->longType;
                    default:
                        return _stb->noType;
                }
            }
            else
            {
                switch (intTypeASTNode->getAlignment())
                {
                    case 1:
                    {
                        STType *type = _stb->allocType(STTK_CHAR);
                        type->size = 1;
                        type->isSigned = false;
                        return type;
                    }
                    case 2:
                    {
                        STType *type = _stb->allocType(STTK_SHORT);
                        type->size = 2;
                        type->isSigned = false;
                        return type;
                    }
                    case 4:
                        return _stb->unsignedType;
                    case 8:
                    {
                        STType *type = _stb->allocType(STTK_LONG);
                        type->size = 8;
                        type->isSigned = false;
                        return type;
                    }
                    default:
                        return _stb->noType;
                }
            }
        }
        break;
        case NK_REAL_TYPE:
        {
            RealTypeASTNode *realTypeASTNode =
                static_cast<RealTypeASTNode *>(typeASTNode);

            return realTypeASTNode->getIsDouble() ? _stb->doubleType
                                                  : _stb->floatType;
        }
        case NK_COMPLEX_TYPE:
            return _stb->noType;
        case NK_ENUMERAL_TYPE:
            return _stb->intType;
        case NK_BOOLEAN_TYPE:
            return _stb->boolType;
        case NK_POINTER_TYPE:
        {
            STType *type = _stb->allocType(STTK_POINTER);
            PointerTypeASTNode *ptrTypeASTNode =
                static_cast<PointerTypeASTNode *>(typeASTNode);
            type->baseType = getStbType(ptrTypeASTNode->getBaseType());
            type->size = 8; // RV64 LP64 pointer.
            return type;
        }
        case NK_REFERENCE_TYPE:
            return _stb->noType;
        case NK_FUNCTION_TYPE:
        {
            STType *type = _stb->allocType(STTK_FUNCTION);
            FunctionTypeASTNode *funcTypeASTNode =
                static_cast<FunctionTypeASTNode *>(typeASTNode);

            type->funcType = getStbType(funcTypeASTNode->getType());
            type->size = 4;
            return type;
        }
        case NK_ARRAY_TYPE:
        {
            STType *type = _stb->allocType(STTK_ARRAY);
            ArrayTypeASTNode *arrTypeASTNode =
                static_cast<ArrayTypeASTNode *>(typeASTNode);
            type->elemType = getStbType(arrTypeASTNode->getElementType());

            if (type->elemType && AST_MATCH_INTEGER_CONST(arrTypeASTNode->getExpr()))
            {
                auto *bound = static_cast<IntegerConstASTNode *>(arrTypeASTNode->getExpr());
                if (!bound->getEnumerator())
                    type->size = type->elemType->size * bound->getValue();
            }

            return type;
        }
        case NK_STRUCT_TYPE:
        {
            STType *type = nullptr;
            // FIXME: At this point a record type should already be in the
            // symbol table.
            StructTypeASTNode *recTypeASTNode =
                static_cast<StructTypeASTNode *>(typeASTNode);
            if (recTypeASTNode->getIdentity())
                return const_cast<STType *>(recTypeASTNode->getIdentity());

            if (AST_MATCH_IDENT(recTypeASTNode->getName()))
            {
                const char *recordName =
                    AST_IDENT_VALUE(recTypeASTNode->getName());
                unsigned recordLineNum =
                    AST_IDENT_LINE_NUM(recTypeASTNode->getName());
                const std::string key = std::string("tag:") + recordName;
                STObject *obj = _stb->find(key.c_str());

                if (obj == _stb->noObj)
                    // Record type declaration is not found in symbol table.
                    error(recordLineNum,
                          "struct type '%s' not declared",
                          recordName);
                else
                    type = obj->type;
            }
            return type;
        }
        case NK_UNION_TYPE:
        {
            STType *type = nullptr;
            // FIXME: At this point a union type should already be in the
            // symbol table.
            UnionTypeASTNode *unionTypeASTNode =
                static_cast<UnionTypeASTNode *>(typeASTNode);
            if (unionTypeASTNode->getIdentity())
                return const_cast<STType *>(unionTypeASTNode->getIdentity());

            if (AST_MATCH_IDENT(unionTypeASTNode->getName()))
            {
                const char *unionName =
                    AST_IDENT_VALUE(unionTypeASTNode->getName());
                unsigned unionLineNum =
                    AST_IDENT_LINE_NUM(unionTypeASTNode->getName());
                const std::string key = std::string("tag:") + unionName;
                STObject *obj = _stb->find(key.c_str());

                if (obj == _stb->noObj)
                    // Union type declaration is not found in symbol table.
                    error(unionLineNum,
                          "union type '%s' not declared",
                          unionName);
                else
                    type = obj->type;
            }
            return type;
        }
        case NK_UNKNOWN_TYPE:
            return _stb->noType;
        default:
            return _stb->noType;
    }
}

void AbstractSyntaxTree::declare()
{
    if (_root->getKind() == NK_LIST)
        _stb->setTopScope(static_cast<SequenceASTNode *>(_root)->getScope());

    _root->declare(this);

    if (_root->getKind() == NK_LIST)
        _stb->setTopScope(nullptr);
}

namespace
{

// Repeated typedefs may denote the same non-variably modified type (6.7p3).
// The vendor parser recognizes fixed array bounds here when they are literals;
// complex bounds are not assumed constant or equivalent.
bool sameTypedefType(ASTNode *left, ASTNode *right, bool ignoreQualifiers = false);

bool sameBound(ASTNode *left, ASTNode *right)
{
    if (left == right)
        return true;
    if (!AST_MATCH_INTEGER_CONST(left) || !AST_MATCH_INTEGER_CONST(right))
        return false;
    auto *lhs = static_cast<IntegerConstASTNode *>(left);
    auto *rhs = static_cast<IntegerConstASTNode *>(right);
    if (lhs->getEnumerator() && lhs->getEnumerator() == rhs->getEnumerator())
        return true;
    const auto isLiteralBound = [](IntegerConstASTNode *node) {
        if (!node->getEnumerator())
            return true;
        ASTNode *init = node->getEnumerator()->getInitializer();
        return AST_MATCH_INTEGER_CONST(init) &&
               !static_cast<IntegerConstASTNode *>(init)->getEnumerator();
    };
    return isLiteralBound(lhs) && isLiteralBound(rhs) &&
           lhs->getValue() == rhs->getValue();
}

ASTNode *adjustedPointee(ASTNode *node)
{
    switch (node->getKind())
    {
        case NK_ARRAY_TYPE:
            return static_cast<ArrayTypeASTNode *>(node)->getElementType();
        case NK_FUNCTION_TYPE:
            return node;
        case NK_POINTER_TYPE:
            return static_cast<PointerTypeASTNode *>(node)->getBaseType();
        default:
            return nullptr;
    }
}

bool sameParameters(FunctionTypeASTNode *left, FunctionTypeASTNode *right)
{
    if (left->hasPrototype() != right->hasPrototype() ||
        left->isVariadic() != right->isVariadic())
        return false;
    if (!left->hasPrototype())
        return true;
    const auto &lhs = static_cast<SequenceASTNode *>(left->getPrms())->getElements();
    const auto &rhs = static_cast<SequenceASTNode *>(right->getPrms())->getElements();
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        auto *a = static_cast<ParmDeclASTNode *>(lhs[i])->getType();
        auto *b = static_cast<ParmDeclASTNode *>(rhs[i])->getType();
        auto *ap = adjustedPointee(a);
        auto *bp = adjustedPointee(b);
        if (ap || bp)
        {
            if (!ap || !bp || !sameTypedefType(ap, bp))
                return false;
        }
        else if (!sameTypedefType(a, b, true))
            return false;
    }
    return true;
}

bool sameTypedefType(ASTNode *left, ASTNode *right, bool ignoreQualifiers)
{
    if (left->getKind() != right->getKind() ||
        (!ignoreQualifiers && ((left->getFlags() ^ right->getFlags()) & DS_QUALIFIER_MASK)))
        return false;
    switch (left->getKind())
    {
        case NK_VOID_TYPE:
        case NK_BOOLEAN_TYPE:
            return true;
        case NK_INTEGRAL_TYPE:
        {
            auto *lhs = static_cast<IntegralTypeASTNode *>(left);
            auto *rhs = static_cast<IntegralTypeASTNode *>(right);
            return lhs->getRank() == rhs->getRank() &&
                   lhs->getAlignment() == rhs->getAlignment() &&
                   lhs->getIsSigned() == rhs->getIsSigned() &&
                   lhs->isPlainChar() == rhs->isPlainChar();
        }
        case NK_REAL_TYPE:
        {
            auto *lhs = static_cast<RealTypeASTNode *>(left);
            auto *rhs = static_cast<RealTypeASTNode *>(right);
            return lhs->getAlignment() == rhs->getAlignment() &&
                   lhs->getIsDouble() == rhs->getIsDouble();
        }
        case NK_POINTER_TYPE:
            return sameTypedefType(static_cast<PointerTypeASTNode *>(left)->getBaseType(),
                                   static_cast<PointerTypeASTNode *>(right)->getBaseType());
        case NK_ARRAY_TYPE:
        {
            auto *lhs = static_cast<ArrayTypeASTNode *>(left);
            auto *rhs = static_cast<ArrayTypeASTNode *>(right);
            return sameBound(lhs->getExpr(), rhs->getExpr()) &&
                   sameTypedefType(lhs->getElementType(), rhs->getElementType());
        }
        case NK_FUNCTION_TYPE:
        {
            auto *lhs = static_cast<FunctionTypeASTNode *>(left);
            auto *rhs = static_cast<FunctionTypeASTNode *>(right);
            return sameTypedefType(lhs->getType(), rhs->getType()) &&
                   sameParameters(lhs, rhs);
        }
        case NK_STRUCT_TYPE:
            return static_cast<StructTypeASTNode *>(left)->getIdentity() ==
                   static_cast<StructTypeASTNode *>(right)->getIdentity();
        case NK_UNION_TYPE:
            return static_cast<UnionTypeASTNode *>(left)->getIdentity() ==
                   static_cast<UnionTypeASTNode *>(right)->getIdentity();
        case NK_ENUMERAL_TYPE:
            return static_cast<EnumeralTypeASTNode *>(left)->getIdentity() ==
                   static_cast<EnumeralTypeASTNode *>(right)->getIdentity();
        default:
            return false;
    }
}

} // namespace

void TypeDeclASTNode::declare(AbstractSyntaxTree *ast)
{
    assert(_name != NULL_AST_NODE && "invalid type declaration");

    SymbolTable *stb = ast->getSymbolTable();
    const char *typeName = AST_IDENT_VALUE(_name);
    unsigned typeNameLineNum = AST_IDENT_LINE_NUM(_name);

    const auto *existing = stb->findLocal(typeName);
    if (existing != stb->noObj)
    {
        if (existing->kind == STOK_TYPE && existing->astType &&
            sameTypedefType(existing->astType, _body))
            return;
        ast->error(typeNameLineNum, "type name '%s' already exists with a different declaration",
                   typeName);
    }
    STType *type = ast->getStbType(_body);
    if (!type)
        type = stb->noType;
    STObject *obj = stb->insert(typeName, STOK_TYPE, type);

    obj->astType = _body;
}

void VarDeclASTNode::declare(AbstractSyntaxTree *ast)
{
    if (!AST_MATCH_IDENT(_name))
        return;
    SymbolTable *stb = ast->getSymbolTable();
    const char *name = AST_IDENT_VALUE(_name);
    const STObject *existing = stb->findLocal(name);
    // The Cephyr lowerer reconciles compatible redeclarations with linkage.
    // At block scope only an extern declaration can be such a redeclaration.
    if ((stb->getLevel() == 0 || (getFlags() & SCS_EXTERN)) && existing != stb->noObj &&
        existing->kind == STOK_VAR)
        return;
    STType *type = ast->getStbType(_type);
    STObject *obj = stb->insert(name, STOK_VAR,
                                type ? type : stb->noType);
    if (obj == stb->noObj)
        ast->error(AST_IDENT_LINE_NUM(_name), "name '%s' is already declared",
                   AST_IDENT_VALUE(_name));
}

void ParmDeclASTNode::declare(AbstractSyntaxTree *ast)
{
    if (!AST_MATCH_IDENT(_name))
        return;
    SymbolTable *stb = ast->getSymbolTable();
    STType *type = ast->getStbType(_type);
    if (stb->insert(AST_IDENT_VALUE(_name), STOK_PAR,
                    type ? type : stb->noType) == stb->noObj)
        ast->error(AST_IDENT_LINE_NUM(_name), "parameter '%s' is already declared",
                   AST_IDENT_VALUE(_name));
}

void FieldDeclASTNode::declare(AbstractSyntaxTree *ast)
{
    if (!AST_MATCH_IDENT(_name))
        return;
    SymbolTable *stb = ast->getSymbolTable();
    STType *type = ast->getStbType(_type);
    if (!type)
        type = stb->noType;
    if (stb->insert(AST_IDENT_VALUE(_name), STOK_VAR, type) == stb->noObj)
        ast->error(AST_IDENT_LINE_NUM(_name), "field '%s' is already declared",
                   AST_IDENT_VALUE(_name));
    else
        stb->getTopScope()->size += type->size;
}

void CompoundStmtASTNode::declare(AbstractSyntaxTree *ast)
{
    if (_decls != NULL_AST_NODE)
        _decls->declare(ast);
}

void SequenceASTNode::declare(AbstractSyntaxTree *ast)
{
    for (unsigned i = 0; i < _elements.size(); i++)
        _elements[i]->declare(ast);
}

} // namespace cparser
