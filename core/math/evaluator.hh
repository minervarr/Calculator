// evaluator.hh — pluggable evaluator interface for the expression AST.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// The parser produces an AST; an IEvaluator consumes it. Start with the numeric
// evaluator; a symbolic (CAS) evaluator can be swapped in later without changing
// the lexer or parser.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "ast.hh"

namespace mathx {

struct EvalContext {
    bool   degrees = false;  // trig in degrees when true, radians otherwise
    double ans     = 0.0;    // value of the previous result (for "Ans")

    // Variable bindings (x, t, θ, … and future user-defined vars). A pointer to a
    // caller-owned list so a plotting loop can rebind a value each sample without
    // allocating. Unbound names evaluate to "Undefined".
    const std::vector<std::pair<std::string, double>>* vars = nullptr;

    bool lookupVar(const std::string& name, double& out) const {
        if (!vars) return false;
        for (const auto& kv : *vars)
            if (kv.first == name) { out = kv.second; return true; }
        return false;
    }
};

struct EvalResult {
    bool        ok    = false;
    double      value = 0.0;
    std::string error;       // "Undefined", "Math Error", ... when !ok
};

class IEvaluator {
public:
    virtual ~IEvaluator() = default;
    virtual EvalResult eval(const Node& root, const EvalContext& ctx) const = 0;
};

}  // namespace mathx
