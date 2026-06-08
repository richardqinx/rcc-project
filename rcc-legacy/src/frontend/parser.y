/* Source code for RCC-Legacy.
   Copyright (C) 2026 Richard Qin

This file is part of RCC.

RCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

RCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with RCC; see the file LICENSE.  If not see
<http://www.gnu.org/licenses/>.  */
%{
/* parser.y - SysY2022 Parser */
#include "rcc/ast.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace rcc;

// Global AST root node
extern CompUnitNode* g_ast_root;

extern int yylex();
extern int yylineno_val;
void yyerror(const char* msg);
extern FILE* yyin;
%}

/* ─── Bison Configuration ─── */
%locations
%define parse.error verbose

/* ─── Value Types ─── */
%union {
    int32_t       int_val;
    float         float_val;
    char*         str_val;
    rcc::ASTNode*      node;
    rcc::ASTList*      node_list;
    rcc::BType         btype;
}

/* ─── Terminals ─── */
%token <int_val>    INT_CONST
%token <float_val>  FLOAT_CONST
%token <str_val>    IDENT STRING_CONST

%token T_INT T_FLOAT T_VOID T_CONST
%token T_IF T_ELSE T_WHILE T_BREAK T_CONTINUE T_RETURN
%token T_AND T_OR T_LE T_GE T_EQ T_NE

/* ─── Non-terminal Types ─── */
%type <node>  CompUnit
%type <node>  GlobalItem FuncDef_Void
%type <node>  FuncFParam
%type <node>  Block BlockItem Stmt
%type <node>  Decl ConstDecl VarDecl ConstDef VarDef
%type <node>  InitVal ConstInitVal
%type <node>  Exp Cond LVal PrimaryExp UnaryExp Number
%type <node>  MulExp AddExp RelExp EqExp LAndExp LOrExp ConstExp
%type <node_list> ConstDefList VarDefList
%type <node_list> FuncFParams FuncRParams
%type <node_list> BlockItemList
%type <node_list> ArrayDims ArrayIndices
%type <node_list> InitValList ConstInitValList
%type <btype> BType

/* ─── Resolve dangling else ─── */
%nonassoc LOWER_THAN_ELSE
%nonassoc T_ELSE

/* ─── Start Symbol ─── */
%start CompUnit

%%

/* Compilation Unit */

CompUnit
    : CompUnit GlobalItem {
        auto* unit = static_cast<CompUnitNode*>($1);
        unit->decls_and_funcs.push_back(ASTPtr($2));
        $$ = unit;
    }
    | CompUnit ConstDecl {
        auto* unit = static_cast<CompUnitNode*>($1);
        unit->decls_and_funcs.push_back(ASTPtr($2));
        $$ = unit;
    }
    | CompUnit FuncDef_Void {
        auto* unit = static_cast<CompUnitNode*>($1);
        unit->decls_and_funcs.push_back(ASTPtr($2));
        $$ = unit;
    }
    | GlobalItem {
        auto* unit = new CompUnitNode();
        unit->decls_and_funcs.push_back(ASTPtr($1));
        $$ = unit;
        g_ast_root = unit;
    }
    | ConstDecl {
        auto* unit = new CompUnitNode();
        unit->decls_and_funcs.push_back(ASTPtr($1));
        $$ = unit;
        g_ast_root = unit;
    }
    | FuncDef_Void {
        auto* unit = new CompUnitNode();
        unit->decls_and_funcs.push_back(ASTPtr($1));
        $$ = unit;
        g_ast_root = unit;
    }
    ;

/* GlobalItem */
GlobalItem
    : BType IDENT '(' ')' Block {
        /* FuncDef: int/float foo() { ... } */
        auto* func = new FuncDefNode();
        func->ret_type = $1;
        func->name = $2; free($2);
        func->body = ASTPtr($5);
        func->lineno = @1.first_line;
        $$ = func;
    }
    | BType IDENT '(' FuncFParams ')' Block {
        /* FuncDef: int/float foo(params) { ... } */
        auto* func = new FuncDefNode();
        func->ret_type = $1;
        func->name = $2; free($2);
        func->params = std::move(*$4); delete $4;
        func->body = ASTPtr($6);
        func->lineno = @1.first_line;
        $$ = func;
    }
    | BType IDENT ArrayDims ';' {
        /* VarDecl: int x; / int x[10]; */
        auto* decl = new VarDeclNode();
        decl->is_const = false;
        decl->btype = $1;
        auto* def = new VarDefNode();
        def->name = $2; free($2);
        def->dims = std::move(*$3); delete $3;
        def->lineno = @2.first_line;
        decl->defs.push_back(ASTPtr(def));
        $$ = decl;
    }
    | BType IDENT ArrayDims '=' InitVal ';' {
        /* VarDecl: int x = 5; / int x[10] = {1,2}; */
        auto* decl = new VarDeclNode();
        decl->is_const = false;
        decl->btype = $1;
        auto* def = new VarDefNode();
        def->name = $2; free($2);
        def->dims = std::move(*$3); delete $3;
        def->init_val = ASTPtr($5);
        def->lineno = @2.first_line;
        decl->defs.push_back(ASTPtr(def));
        $$ = decl;
    }
    | BType IDENT ArrayDims ',' VarDefList ';' {
        /* VarDecl: int x, y, z; */
        auto* decl = new VarDeclNode();
        decl->is_const = false;
        decl->btype = $1;
        auto* def = new VarDefNode();
        def->name = $2; free($2);
        def->dims = std::move(*$3); delete $3;
        def->lineno = @2.first_line;
        decl->defs.push_back(ASTPtr(def));
        for (auto& d : *$5) decl->defs.push_back(std::move(d));
        delete $5;
        $$ = decl;
    }
    | BType IDENT ArrayDims '=' InitVal ',' VarDefList ';' {
        /* VarDecl: int x = 1, y, z; */
        auto* decl = new VarDeclNode();
        decl->is_const = false;
        decl->btype = $1;
        auto* def = new VarDefNode();
        def->name = $2; free($2);
        def->dims = std::move(*$3); delete $3;
        def->init_val = ASTPtr($5);
        def->lineno = @2.first_line;
        decl->defs.push_back(ASTPtr(def));
        for (auto& d : *$7) decl->defs.push_back(std::move(d));
        delete $7;
        $$ = decl;
    }
    ;

/* void functions */
FuncDef_Void
    : T_VOID IDENT '(' ')' Block {
        auto* func = new FuncDefNode();
        func->ret_type = BType::Void;
        func->name = $2; free($2);
        func->body = ASTPtr($5);
        func->lineno = @1.first_line;
        $$ = func;
    }
    | T_VOID IDENT '(' FuncFParams ')' Block {
        auto* func = new FuncDefNode();
        func->ret_type = BType::Void;
        func->name = $2; free($2);
        func->params = std::move(*$4); delete $4;
        func->body = ASTPtr($6);
        func->lineno = @1.first_line;
        $$ = func;
    }
    ;

/* Types */

BType : T_INT   { $$ = BType::Int; }
      | T_FLOAT { $$ = BType::Float; }
      ;

/* Declarations */

/* Decl */
Decl : ConstDecl { $$ = $1; }
     | VarDecl   { $$ = $1; }
     ;

ConstDecl
    : T_CONST BType ConstDefList ';' {
        auto* decl = new VarDeclNode();
        decl->is_const = true;
        decl->btype = $2;
        decl->defs = std::move(*$3);
        delete $3;
        $$ = decl;
    }
    ;

ConstDefList
    : ConstDef {
        $$ = new ASTList();
        $$->push_back(ASTPtr($1));
    }
    | ConstDefList ',' ConstDef {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

ConstDef
    : IDENT ArrayDims '=' ConstInitVal {
        auto* def = new VarDefNode();
        def->name = $1; free($1);
        def->dims = std::move(*$2); delete $2;
        def->init_val = ASTPtr($4);
        def->lineno = @1.first_line;
        $$ = def;
    }
    ;

ConstInitVal
    : ConstExp { $$ = $1; }
    | '{' '}' {
        $$ = new InitListNode();
    }
    | '{' ConstInitValList '}' {
        auto* list = new InitListNode();
        list->items = std::move(*$2); delete $2;
        $$ = list;
    }
    ;

ConstInitValList
    : ConstInitVal {
        $$ = new ASTList();
        $$->push_back(ASTPtr($1));
    }
    | ConstInitValList ',' ConstInitVal {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

/* Local VarDecl */
VarDecl
    : BType VarDefList ';' {
        auto* decl = new VarDeclNode();
        decl->is_const = false;
        decl->btype = $1;
        decl->defs = std::move(*$2);
        delete $2;
        $$ = decl;
    }
    ;

VarDefList
    : VarDef {
        $$ = new ASTList();
        $$->push_back(ASTPtr($1));
    }
    | VarDefList ',' VarDef {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

VarDef
    : IDENT ArrayDims {
        auto* def = new VarDefNode();
        def->name = $1; free($1);
        def->dims = std::move(*$2); delete $2;
        def->lineno = @1.first_line;
        $$ = def;
    }
    | IDENT ArrayDims '=' InitVal {
        auto* def = new VarDefNode();
        def->name = $1; free($1);
        def->dims = std::move(*$2); delete $2;
        def->init_val = ASTPtr($4);
        def->lineno = @1.first_line;
        $$ = def;
    }
    ;

InitVal
    : Exp { $$ = $1; }
    | '{' '}' {
        $$ = new InitListNode();
    }
    | '{' InitValList '}' {
        auto* list = new InitListNode();
        list->items = std::move(*$2); delete $2;
        $$ = list;
    }
    ;

InitValList
    : InitVal {
        $$ = new ASTList();
        $$->push_back(ASTPtr($1));
    }
    | InitValList ',' InitVal {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

/* Array Dimensions */
ArrayDims
    : /* empty */ { $$ = new ASTList(); }
    | ArrayDims '[' ConstExp ']' {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

/* Function Parameters */

FuncFParams
    : FuncFParam {
        $$ = new ASTList();
        $$->push_back(ASTPtr($1));
    }
    | FuncFParams ',' FuncFParam {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

FuncFParam
    : BType IDENT {
        auto* p = new FuncFParamNode();
        p->btype = $1;
        p->name = $2; free($2);
        p->lineno = @1.first_line;
        $$ = p;
    }
    | BType IDENT '[' ']' ArrayDims {
        /* Array parameter */
        auto* p = new FuncFParamNode();
        p->btype = $1;
        p->name = $2; free($2);
        p->is_array = true;
        p->dims = std::move(*$5); delete $5;
        p->lineno = @1.first_line;
        $$ = p;
    }
    ;

/* Blocks and Statements */

Block
    : '{' BlockItemList '}' {
        auto* block = new BlockNode();
        block->items = std::move(*$2); delete $2;
        $$ = block;
    }
    ;

BlockItemList
    : /* empty */ { $$ = new ASTList(); }
    | BlockItemList BlockItem {
        $1->push_back(ASTPtr($2));
        $$ = $1;
    }
    ;

BlockItem
    : Decl { $$ = $1; }
    | Stmt { $$ = $1; }
    ;

Stmt
    : LVal '=' Exp ';' {
        auto* s = new AssignStmtNode();
        s->lval = ASTPtr($1);
        s->expr = ASTPtr($3);
        s->lineno = @1.first_line;
        $$ = s;
    }
    | Exp ';' {
        auto* s = new ExprStmtNode();
        s->expr = ASTPtr($1);
        $$ = s;
    }
    | ';' {
        $$ = new ExprStmtNode();
    }
    | Block { $$ = $1; }
    | T_IF '(' Cond ')' Stmt %prec LOWER_THAN_ELSE {
        auto* s = new IfStmtNode();
        s->cond = ASTPtr($3);
        s->then_stmt = ASTPtr($5);
        s->lineno = @1.first_line;
        $$ = s;
    }
    | T_IF '(' Cond ')' Stmt T_ELSE Stmt {
        auto* s = new IfStmtNode();
        s->cond = ASTPtr($3);
        s->then_stmt = ASTPtr($5);
        s->else_stmt = ASTPtr($7);
        s->lineno = @1.first_line;
        $$ = s;
    }
    | T_WHILE '(' Cond ')' Stmt {
        auto* s = new WhileStmtNode();
        s->cond = ASTPtr($3);
        s->body = ASTPtr($5);
        s->lineno = @1.first_line;
        $$ = s;
    }
    | T_BREAK ';' {
        $$ = new BreakStmtNode();
        $$->lineno = @1.first_line;
    }
    | T_CONTINUE ';' {
        $$ = new ContinueStmtNode();
        $$->lineno = @1.first_line;
    }
    | T_RETURN ';' {
        $$ = new ReturnStmtNode();
        $$->lineno = @1.first_line;
    }
    | T_RETURN Exp ';' {
        auto* s = new ReturnStmtNode();
        s->expr = ASTPtr($2);
        s->lineno = @1.first_line;
        $$ = s;
    }
    ;

/* Expressions */

/* Exp relaxed to LOrExp */
Exp : LOrExp { $$ = $1; } ;

Cond : LOrExp { $$ = $1; } ;

ConstExp : AddExp { $$ = $1; } ;

LVal
    : IDENT ArrayIndices {
        auto* lv = new LValNode();
        lv->name = $1; free($1);
        lv->indices = std::move(*$2); delete $2;
        lv->lineno = @1.first_line;
        $$ = lv;
    }
    ;

ArrayIndices
    : /* empty */ { $$ = new ASTList(); }
    | ArrayIndices '[' Exp ']' {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    ;

PrimaryExp
    : '(' Exp ')' { $$ = $2; }
    | LVal        { $$ = $1; }
    | Number      { $$ = $1; }
    ;

Number
    : INT_CONST {
        auto* n = new IntLiteralNode();
        n->value = $1;
        n->lineno = @1.first_line;
        $$ = n;
    }
    | FLOAT_CONST {
        auto* n = new FloatLiteralNode();
        n->value = $1;
        n->lineno = @1.first_line;
        $$ = n;
    }
    ;

UnaryExp
    : PrimaryExp { $$ = $1; }
    | IDENT '(' ')' {
        auto* c = new FuncCallNode();
        c->name = $1; free($1);
        c->lineno = @1.first_line;
        $$ = c;
    }
    | IDENT '(' FuncRParams ')' {
        auto* c = new FuncCallNode();
        c->name = $1; free($1);
        c->args = std::move(*$3); delete $3;
        c->lineno = @1.first_line;
        $$ = c;
    }
    | '+' UnaryExp {
        auto* u = new UnaryExprNode();
        u->op = UnaryOp::Pos;
        u->operand = ASTPtr($2);
        u->lineno = @1.first_line;
        $$ = u;
    }
    | '-' UnaryExp {
        auto* u = new UnaryExprNode();
        u->op = UnaryOp::Neg;
        u->operand = ASTPtr($2);
        u->lineno = @1.first_line;
        $$ = u;
    }
    | '!' UnaryExp {
        auto* u = new UnaryExprNode();
        u->op = UnaryOp::Not;
        u->operand = ASTPtr($2);
        u->lineno = @1.first_line;
        $$ = u;
    }
    ;

FuncRParams
    : Exp {
        $$ = new ASTList();
        $$->push_back(ASTPtr($1));
    }
    | FuncRParams ',' Exp {
        $1->push_back(ASTPtr($3));
        $$ = $1;
    }
    /* putf format string parameter */
    | STRING_CONST {
        $$ = new ASTList();
        auto* n = new IntLiteralNode();  /* placeholder */
        n->value = 0;
        $$->push_back(ASTPtr(static_cast<ASTNode*>(n)));
        free($1);
    }
    | FuncRParams ',' STRING_CONST {
        free($3);
        $$ = $1;
    }
    ;

MulExp
    : UnaryExp { $$ = $1; }
    | MulExp '*' UnaryExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Mul;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | MulExp '/' UnaryExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Div;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | MulExp '%' UnaryExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Mod;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    ;

AddExp
    : MulExp { $$ = $1; }
    | AddExp '+' MulExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Add;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | AddExp '-' MulExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Sub;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    ;

RelExp
    : AddExp { $$ = $1; }
    | RelExp '<' AddExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Lt;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | RelExp '>' AddExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Gt;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | RelExp T_LE AddExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Le;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | RelExp T_GE AddExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Ge;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    ;

EqExp
    : RelExp { $$ = $1; }
    | EqExp T_EQ RelExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Eq;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    | EqExp T_NE RelExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Ne;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    ;

LAndExp
    : EqExp { $$ = $1; }
    | LAndExp T_AND EqExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::And;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    ;

LOrExp
    : LAndExp { $$ = $1; }
    | LOrExp T_OR LAndExp {
        auto* e = new BinaryExprNode(); e->op = BinOp::Or;
        e->lhs = ASTPtr($1); e->rhs = ASTPtr($3);
        e->lineno = @2.first_line; $$ = e;
    }
    ;

%%

/* Global Variables and Error Handling */
CompUnitNode* g_ast_root = nullptr;

void yyerror(const char* msg) {
    fprintf(stderr, "rcc: parse error (line %d): %s\n", yylineno_val, msg);
}
