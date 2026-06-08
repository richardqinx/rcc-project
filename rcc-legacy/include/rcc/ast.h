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

#pragma once

/// @file ast.h
/// @brief SysY AST Node Definitions

#include "rcc/sysy_type.h"

#include <memory>
#include <string>
#include <vector>

namespace rcc {

// Forward declarations of all AST nodes
struct ASTNode;
struct CompUnitNode;
struct FuncDefNode;
struct FuncFParamNode;
struct VarDeclNode;
struct VarDefNode;
struct BlockNode;
struct AssignStmtNode;
struct ExprStmtNode;
struct IfStmtNode;
struct WhileStmtNode;
struct BreakStmtNode;
struct ContinueStmtNode;
struct ReturnStmtNode;
struct IntLiteralNode;
struct FloatLiteralNode;
struct LValNode;
struct FuncCallNode;
struct BinaryExprNode;
struct UnaryExprNode;
struct ImplicitCastNode;
struct InitListNode;

using ASTPtr = std::unique_ptr<ASTNode>;
using ASTList = std::vector<ASTPtr>;

// operators

enum class BinOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Lt,
    Gt,
    Le,
    Ge,
    Eq,
    Ne,
    And,
    Or,
};

enum class UnaryOp { Pos, Neg, Not };

// AST Node Kind

enum class NodeKind {
    CompUnit,
    FuncDef,
    FuncFParam,
    VarDecl,
    VarDef,
    Block,
    AssignStmt,
    ExprStmt,
    IfStmt,
    WhileStmt,
    BreakStmt,
    ContinueStmt,
    ReturnStmt,
    IntLiteral,
    FloatLiteral,
    LVal,
    FuncCall,
    UnaryExpr,
    BinaryExpr,
    ImplicitCast,
    InitList,
};

// AST Node Base Class

struct ASTNode {
    NodeKind kind;
    BType eval_type = BType::Int;
    int lineno = 0;

    explicit ASTNode(NodeKind k) : kind(k) {}
    virtual ~ASTNode() = default;

    // no copy
    ASTNode(const ASTNode&) = delete;
    ASTNode& operator=(const ASTNode&) = delete;
    ASTNode(ASTNode&&) = default;
    ASTNode& operator=(ASTNode&&) = default;
};

// Top-level Nodes

struct CompUnitNode : ASTNode {
    ASTList decls_and_funcs;

    CompUnitNode() : ASTNode(NodeKind::CompUnit) {}
};

struct FuncDefNode : ASTNode {
    BType ret_type = BType::Void;
    std::string name;
    ASTList params;  // FuncFParamNode
    ASTPtr body;     // BlockNode

    FuncDefNode() : ASTNode(NodeKind::FuncDef) {}
};

struct FuncFParamNode : ASTNode {
    BType btype = BType::Int;
    std::string name;
    bool is_array = false;
    ASTList dims;  // first dim is empty, others are ConstExp

    FuncFParamNode() : ASTNode(NodeKind::FuncFParam) {}
};

// declarations

struct VarDeclNode : ASTNode {
    bool is_const = false;
    BType btype = BType::Int;
    ASTList defs;  // VarDefNode

    VarDeclNode() : ASTNode(NodeKind::VarDecl) {}
};

struct VarDefNode : ASTNode {
    std::string name;
    ASTList dims;     // each dim size (ConstExp), scalar has empty dims list
    ASTPtr init_val;  // InitListNode | Expr | nullptr

    VarDefNode() : ASTNode(NodeKind::VarDef) {}
};

struct InitListNode : ASTNode {
    ASTList items;  // Expr or InitListNode

    InitListNode() : ASTNode(NodeKind::InitList) {}
};

// statements

struct BlockNode : ASTNode {
    ASTList items;  // Decl | Stmt

    BlockNode() : ASTNode(NodeKind::Block) {}
};

struct AssignStmtNode : ASTNode {
    ASTPtr lval;  // LValNode
    ASTPtr expr;

    AssignStmtNode() : ASTNode(NodeKind::AssignStmt) {}
};

struct ExprStmtNode : ASTNode {
    ASTPtr expr;  // nullable (empty statement)

    ExprStmtNode() : ASTNode(NodeKind::ExprStmt) {}
};

struct IfStmtNode : ASTNode {
    ASTPtr cond;
    ASTPtr then_stmt;
    ASTPtr else_stmt;  // nullable

    IfStmtNode() : ASTNode(NodeKind::IfStmt) {}
};

struct WhileStmtNode : ASTNode {
    ASTPtr cond;
    ASTPtr body;

    WhileStmtNode() : ASTNode(NodeKind::WhileStmt) {}
};

struct BreakStmtNode : ASTNode {
    BreakStmtNode() : ASTNode(NodeKind::BreakStmt) {}
};

struct ContinueStmtNode : ASTNode {
    ContinueStmtNode() : ASTNode(NodeKind::ContinueStmt) {}
};

struct ReturnStmtNode : ASTNode {
    ASTPtr expr;  // nullable for void return

    ReturnStmtNode() : ASTNode(NodeKind::ReturnStmt) {}
};

// expressions

struct IntLiteralNode : ASTNode {
    int32_t value = 0;

    IntLiteralNode() : ASTNode(NodeKind::IntLiteral) { eval_type = BType::Int; }
};

struct FloatLiteralNode : ASTNode {
    float value = 0.0f;

    FloatLiteralNode() : ASTNode(NodeKind::FloatLiteral) { eval_type = BType::Float; }
};

struct LValNode : ASTNode {
    std::string name;
    ASTList indices;  // index expressions

    LValNode() : ASTNode(NodeKind::LVal) {}
};

struct FuncCallNode : ASTNode {
    std::string name;
    ASTList args;

    FuncCallNode() : ASTNode(NodeKind::FuncCall) {}
};

struct BinaryExprNode : ASTNode {
    BinOp op;
    ASTPtr lhs, rhs;

    BinaryExprNode() : ASTNode(NodeKind::BinaryExpr) {}
};

struct UnaryExprNode : ASTNode {
    UnaryOp op;
    ASTPtr operand;

    UnaryExprNode() : ASTNode(NodeKind::UnaryExpr) {}
};

struct ImplicitCastNode : ASTNode {
    BType target_type;
    ASTPtr operand;

    ImplicitCastNode() : ASTNode(NodeKind::ImplicitCast) {}
};

// AST utility functions

/// Print AST (for debugging)
void ast_dump(const ASTNode* node, int indent = 0);

}  // namespace rcc
