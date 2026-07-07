// numeric_evaluator.cc — double-precision numeric evaluator.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "numeric_evaluator.hh"

#include <cmath>

namespace mathx {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kE  = 2.71828182845904523536;

EvalResult ok(double v) { return {true, v, {}}; }
EvalResult err(const std::string& m) { return {false, 0.0, m}; }

EvalResult evalNode(const Node& n, const EvalContext& ctx) {
    switch (n.kind) {
        case Node::Num:
            return ok(n.num);
        case Node::Const:
            return ok(n.name == "pi" ? kPi : kE);
        case Node::Var: {
            double v;
            if (ctx.lookupVar(n.name, v)) return ok(v);
            return err("Undefined");
        }
        case Node::Neg: {
            EvalResult a = evalNode(*n.kids[0], ctx);
            return a.ok ? ok(-a.value) : a;
        }
        case Node::Add: case Node::Sub: case Node::Mul:
        case Node::Div: case Node::Pow: {
            EvalResult a = evalNode(*n.kids[0], ctx);
            if (!a.ok) return a;
            EvalResult b = evalNode(*n.kids[1], ctx);
            if (!b.ok) return b;
            switch (n.kind) {
                case Node::Add: return ok(a.value + b.value);
                case Node::Sub: return ok(a.value - b.value);
                case Node::Mul: return ok(a.value * b.value);
                case Node::Div:
                    if (b.value == 0.0) return err("Undefined");
                    return ok(a.value / b.value);
                case Node::Pow: {
                    double r = std::pow(a.value, b.value);
                    if (std::isnan(r) || std::isinf(r)) return err("Math Error");
                    return ok(r);
                }
                default: break;
            }
            return err("Syntax Error");
        }
        case Node::Call: {
            EvalResult a = evalNode(*n.kids[0], ctx);
            if (!a.ok) return a;
            double x = a.value;
            const std::string& f = n.name;
            if (f == "sin" || f == "cos" || f == "tan") {
                double rad = ctx.degrees ? x * kPi / 180.0 : x;
                double r = (f == "sin") ? std::sin(rad)
                         : (f == "cos") ? std::cos(rad)
                                        : std::tan(rad);
                if (std::isnan(r) || std::isinf(r)) return err("Math Error");
                // π is irrational, so sin(π) etc. can't be exactly 0 in binary —
                // it comes out as representation noise (~1e-16). A calculator
                // should show 0, like every hardware calculator does.
                if (std::fabs(r) < 1e-12) r = 0.0;
                return ok(r);
            }
            if (f == "sqrt") {
                if (x < 0.0) return err("Math Error");
                return ok(std::sqrt(x));
            }
            if (f == "ln") {
                if (x <= 0.0) return err("Math Error");
                return ok(std::log(x));
            }
            if (f == "log") {
                if (x <= 0.0) return err("Math Error");
                return ok(std::log10(x));
            }
            return err("Syntax Error");
        }
    }
    return err("Syntax Error");
}

}  // namespace

EvalResult NumericEvaluator::eval(const Node& root, const EvalContext& ctx) const {
    EvalResult r = evalNode(root, ctx);
    if (r.ok && (std::isnan(r.value) || std::isinf(r.value))) return err("Math Error");
    return r;
}

}  // namespace mathx
