// ast.hh — expression AST for the calculator math core.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Decoupled from any UI/Vulkan/Android code: a pluggable IEvaluator consumes
// this tree, so a future symbolic (CAS) evaluator can replace the numeric one
// without touching the lexer or parser.
#pragma once
#include <memory>
#include <string>
#include <vector>

namespace mathx {

struct Node {
    enum Kind { Num, Const, Var, Neg, Add, Sub, Mul, Div, Pow, Call };
    Kind        kind;
    double      num = 0.0;   // valid for Num
    std::string name;        // Const ("pi","e"), Var ("x","t",θ,…), Call ("sin",…)
    std::vector<std::unique_ptr<Node>> kids;  // operands / call argument

    explicit Node(Kind k) : kind(k) {}
};

using NodePtr = std::unique_ptr<Node>;

inline NodePtr makeNum(double v) {
    auto n = std::make_unique<Node>(Node::Num);
    n->num = v;
    return n;
}

}  // namespace mathx
