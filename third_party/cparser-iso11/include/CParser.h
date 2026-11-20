// C parser - header file.
// Copyright (C) 2017, 2018  Jozef Kolek <jkolek@gmail.com>
//
// All rights reserved.
//
// See the LICENSE file for more details.

#ifndef CPARSER_H
#define CPARSER_H

#include <map>

#include "AbstractSyntaxTree.h"
#include "Parser.h"
#include "Lexer.h"
#include "CLexer.h"

namespace cparser
{

class CParser : public Parser
{
    unsigned _depth = 0;
    unsigned _loopDepth = 0;
    std::vector<bool> _switchDefaults;
    std::map<std::string, Token> _labels;
    std::vector<Token> _gotos;
    ASTNode *_returnType = nullptr;

    enum class DeclaratorName { Required, Optional, Forbidden };
    struct DeclaratorPart
    {
        enum Kind { Pointer, Array, Function } kind;
        int line;
        int column;
        unsigned qualifiers = 0;
        ASTNode *operand = NULL_AST_NODE; // Array bound or function parameters
        STScope *scope = nullptr;
        bool variadic = false;
    };

    class ParseDepth
    {
        CParser &parser;
    public:
        explicit ParseDepth(CParser &p) : parser(p)
        {
            if (parser._depth == 512)
                parser.parsingError("C syntax nesting limit exceeded");
            ++parser._depth;
        }
        ~ParseDepth() { --parser._depth; }
    };

    void initNames();

    bool isTypeSpecifier(unsigned _kind, int n);
    bool isTypeQualifier(unsigned kind);
    bool isStorageClassSpecifier(unsigned kind);
    bool startsDeclaration();

    // THE PARSER RULES

    // Expressions
    ASTNode *PrimaryExpression();
    ASTNode *GenericSelection();
    ASTNode *GenericAssocList();
    ASTNode *GenericAssociation();
    ASTNode *parsePostfixExpression(ASTNode *expr);
    ASTNode *PostfixExpression(ASTNode *typeName);
    ASTNode *ArgumentExpressionList();
    ASTNode *UnaryExpression(ASTNode *typeName);
    ASTNodeKind UnaryOperator();
    ASTNode *CastExpression();
    ASTNode *MultiplicativeExpression();
    ASTNode *AdditiveExpression();
    ASTNode *ShiftExpression();
    ASTNode *RelationalExpression();
    ASTNode *EqualityExpression();
    ASTNode *AndExpression();
    ASTNode *ExclusiveOrExpression();
    ASTNode *InclusiveOrExpression();
    ASTNode *LogicalAndExpression();
    ASTNode *LogicalOrExpression();
    ASTNode *ConditionalExpression();
    ASTNode *AssignmentExpression();
    ASTNode *Expression();
    ASTNode *ConstantExpression();

    // Declarations
    ASTNode *Declaration(ASTNode *declSpec);
    ASTNode *TypedefDeclaration(ASTNode *declSpec);
    ASTNode *StaticAssertDeclaration();
    ASTNode *DeclarationSpecifiers(bool typeOnly = false);
    SequenceASTNode *InitDeclaratorList(ASTNode *typeSpec);
    ASTNode *InitDeclarator(ASTNode *typeSpec);
    ASTNode *InitDeclaratorAfterName(ASTNode *typeSpec, ASTNode *type,
                                    ASTNode *name, STScope *parameterScope,
                                    bool allowDefinition = false);
    void StorageClassSpecifier(unsigned &flags);
    ASTNode *TypeSpecifier();
    ASTNode *StructOrUnionSpecifier();
    STType *TagType(const Token &, STTypeKind, bool defining, bool forward);
    SequenceASTNode *StructDeclarationList();
    ASTNode *StructDeclaration();
    ASTNode *SpecifierQualifierList();
    SequenceASTNode *StructDeclaratorList(ASTNode *typeSpec);
    ASTNode *StructDeclarator(ASTNode *typeSpec);
    ASTNode *EnumSpecifier();
    SequenceASTNode *EnumeratorList();
    void TypeQualifier(unsigned &flags);
    void FunctionSpecifier();
    ASTNode *Declarator(ASTNode * &typeSpec,
                        DeclaratorName mode = DeclaratorName::Required,
                        STScope **parameterScope = nullptr);
    ASTNode *DeclaratorParts(std::vector<DeclaratorPart> &, DeclaratorName);
    ASTNode *GccDeclaratorExtension();
    ASTNode *GccAttributeSpecifier();
    ASTNode *GccAttributeList();
    ASTNode *GccAttribute();
    ASTNode *ParameterTypeList(bool &variadic);
    ASTNode *ParameterList();
    ASTNode *ParameterDeclaration();
    ASTNode *IdentifierList();
    ASTNode *TypeName();
    ASTNode *Initializer();
    ASTNode *InitializerList();

    // Statements
    ASTNode *Statement();
    ASTNode *ScopedStatement();
    ASTNode *LoopBody();
    ASTNode *LabeledStatement();
    ASTNode *CompoundStatement();
    ASTNode *FunctionBody();
    ASTNode *DeclarationList();
    ASTNode *StmtOrDeclList();
    ASTNode *SelectionStatement();
    ASTNode *IterationStatement();
    ASTNode *JumpStatement();
    ASTNode *ExpressionStatement();
    ASTNode *TranslationUnit();
    ASTNode *ExternalDeclaration();
    ASTNode *FunctionDeclaration(FunctionTypeASTNode *type, ASTNode *name,
                                 unsigned flags, STScope *parameterScope,
                                 bool allowDefinition);
public:
    void parse(const char *output);

    CParser(CLexer *LEX) : Parser(LEX)
    {
        initNames();
    }
};

} // namespace cparser

#endif
