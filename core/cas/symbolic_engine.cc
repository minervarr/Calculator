// symbolic_engine.cc — minimal real symbolic differentiation / simplification.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "cas/symbolic_engine.hh"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "math/ast.hh"
#include "math/lexer.hh"
#include "math/numeric_evaluator.hh"
#include "math/parser.hh"

namespace cas {
namespace {

using mathx::Node;
using mathx::NodePtr;

// ── AST construction helpers ────────────────────────────────────────────────
NodePtr num(double v) { return mathx::makeNum(v); }
NodePtr var(const std::string& n) { auto p = std::make_unique<Node>(Node::Var);  p->name = n; return p; }
NodePtr un(Node::Kind k, NodePtr a) {
    auto p = std::make_unique<Node>(k);
    p->kids.push_back(std::move(a));
    return p;
}
NodePtr bin(Node::Kind k, NodePtr a, NodePtr b) {
    auto p = std::make_unique<Node>(k);
    p->kids.push_back(std::move(a));
    p->kids.push_back(std::move(b));
    return p;
}
NodePtr call(const std::string& n, NodePtr a) {
    auto p = std::make_unique<Node>(Node::Call);
    p->name = n;
    p->kids.push_back(std::move(a));
    return p;
}
NodePtr clone(const Node& n) {
    auto p = std::make_unique<Node>(n.kind);
    p->num = n.num;
    p->name = n.name;
    for (const auto& k : n.kids) p->kids.push_back(clone(*k));
    return p;
}

bool isNum(const Node& n, double& v) { if (n.kind == Node::Num) { v = n.num; return true; } return false; }
bool isLit(const Node& n, double v) { return n.kind == Node::Num && n.num == v; }

bool contains(const Node& n, const std::string& x) {
    if (n.kind == Node::Var) return n.name == x;
    for (const auto& k : n.kids) if (contains(*k, x)) return true;
    return false;
}

// Fold f(literal) when it is safe (finite, in-domain). Returns false to keep it
// symbolic. Calculus is radians-native, so no degree conversion here.
bool foldCall(const std::string& f, double a, double& out) {
    double r;
    if      (f == "sin")  r = std::sin(a);
    else if (f == "cos")  r = std::cos(a);
    else if (f == "tan")  r = std::tan(a);
    else if (f == "ln")   { if (a <= 0) return false; r = std::log(a); }
    else if (f == "log")  { if (a <= 0) return false; r = std::log10(a); }
    else if (f == "sqrt") { if (a <  0) return false; r = std::sqrt(a); }
    else return false;
    if (!std::isfinite(r)) return false;
    out = r;
    return true;
}

// ── Simplification (bottom-up: fold constants, apply 0/1 identities) ─────────
NodePtr simplify(const Node& n) {
    switch (n.kind) {
        case Node::Num:   return num(n.num);
        case Node::Const: { auto p = std::make_unique<Node>(Node::Const); p->name = n.name; return p; }
        case Node::Var:   return var(n.name);
        case Node::Neg: {
            NodePtr a = simplify(*n.kids[0]);
            double v;
            if (isNum(*a, v))          return num(-v);
            if (a->kind == Node::Neg)  return clone(*a->kids[0]);   // --u = u
            return un(Node::Neg, std::move(a));
        }
        case Node::Add: {
            NodePtr a = simplify(*n.kids[0]), b = simplify(*n.kids[1]);
            double va, vb;
            if (isNum(*a, va) && isNum(*b, vb)) return num(va + vb);
            if (isLit(*a, 0)) return b;
            if (isLit(*b, 0)) return a;
            return bin(Node::Add, std::move(a), std::move(b));
        }
        case Node::Sub: {
            NodePtr a = simplify(*n.kids[0]), b = simplify(*n.kids[1]);
            double va, vb;
            if (isNum(*a, va) && isNum(*b, vb)) return num(va - vb);
            if (isLit(*b, 0)) return a;
            if (isLit(*a, 0)) return un(Node::Neg, std::move(b));
            return bin(Node::Sub, std::move(a), std::move(b));
        }
        case Node::Mul: {
            NodePtr a = simplify(*n.kids[0]), b = simplify(*n.kids[1]);
            double va, vb;
            if (isNum(*a, va) && isNum(*b, vb)) return num(va * vb);
            if (isLit(*a, 0) || isLit(*b, 0))   return num(0);
            if (isLit(*a, 1)) return b;
            if (isLit(*b, 1)) return a;
            return bin(Node::Mul, std::move(a), std::move(b));
        }
        case Node::Div: {
            NodePtr a = simplify(*n.kids[0]), b = simplify(*n.kids[1]);
            double va, vb;
            if (isNum(*a, va) && isNum(*b, vb) && vb != 0) return num(va / vb);
            if (isLit(*a, 0)) return num(0);
            if (isLit(*b, 1)) return a;
            return bin(Node::Div, std::move(a), std::move(b));
        }
        case Node::Pow: {
            NodePtr a = simplify(*n.kids[0]), b = simplify(*n.kids[1]);
            double va, vb;
            if (isNum(*a, va) && isNum(*b, vb)) return num(std::pow(va, vb));
            if (isLit(*b, 0)) return num(1);
            if (isLit(*b, 1)) return a;
            if (isLit(*a, 1)) return num(1);
            return bin(Node::Pow, std::move(a), std::move(b));
        }
        case Node::Call: {
            NodePtr a = simplify(*n.kids[0]);
            double v, r;
            if (isNum(*a, v) && foldCall(n.name, v, r)) return num(r);
            return call(n.name, std::move(a));
        }
    }
    return clone(n);
}

// ── Differentiation (chain rule throughout) ─────────────────────────────────
struct Differ {
    std::string x;
    bool        ok = true;
    std::string err;

    NodePtr go(const Node& n) {
        switch (n.kind) {
            case Node::Num:
            case Node::Const: return num(0);
            case Node::Var:   return num(n.name == x ? 1.0 : 0.0);
            case Node::Neg:   return un(Node::Neg, go(*n.kids[0]));
            case Node::Add:   return bin(Node::Add, go(*n.kids[0]), go(*n.kids[1]));
            case Node::Sub:   return bin(Node::Sub, go(*n.kids[0]), go(*n.kids[1]));
            case Node::Mul: {  // u'v + uv'
                const Node& u = *n.kids[0]; const Node& v = *n.kids[1];
                return bin(Node::Add,
                           bin(Node::Mul, go(u), clone(v)),
                           bin(Node::Mul, clone(u), go(v)));
            }
            case Node::Div: {  // (u'v - uv') / v^2
                const Node& u = *n.kids[0]; const Node& v = *n.kids[1];
                NodePtr top = bin(Node::Sub,
                                  bin(Node::Mul, go(u), clone(v)),
                                  bin(Node::Mul, clone(u), go(v)));
                return bin(Node::Div, std::move(top), bin(Node::Pow, clone(v), num(2)));
            }
            case Node::Pow:  return powRule(n);
            case Node::Call: return callRule(n);
        }
        return num(0);
    }

    NodePtr powRule(const Node& n) {
        const Node& u = *n.kids[0]; const Node& v = *n.kids[1];
        if (!contains(v, x)) {  // constant exponent: c·u^(c-1)·u'
            NodePtr powTerm = bin(Node::Pow, clone(u), bin(Node::Sub, clone(v), num(1)));
            return bin(Node::Mul, bin(Node::Mul, clone(v), std::move(powTerm)), go(u));
        }
        if (!contains(u, x)) {  // constant base: u^v·ln(u)·v'
            return bin(Node::Mul,
                       bin(Node::Mul, clone(n), call("ln", clone(u))), go(v));
        }
        // General: u^v · (v'·ln(u) + v·u'/u)
        NodePtr factor = bin(Node::Add,
                             bin(Node::Mul, go(v), call("ln", clone(u))),
                             bin(Node::Mul, clone(v), bin(Node::Div, go(u), clone(u))));
        return bin(Node::Mul, clone(n), std::move(factor));
    }

    NodePtr callRule(const Node& n) {
        const Node& u = *n.kids[0];
        const std::string& f = n.name;
        NodePtr d;  // f'(u)
        if      (f == "sin")  d = call("cos", clone(u));
        else if (f == "cos")  d = un(Node::Neg, call("sin", clone(u)));
        else if (f == "tan")  d = bin(Node::Div, num(1), bin(Node::Pow, call("cos", clone(u)), num(2)));
        else if (f == "ln")   d = bin(Node::Div, num(1), clone(u));
        else if (f == "log")  d = bin(Node::Div, num(1), bin(Node::Mul, clone(u), call("ln", num(10))));
        else if (f == "sqrt") d = bin(Node::Div, num(1), bin(Node::Mul, num(2), call("sqrt", clone(u))));
        else { ok = false; err = "Unsupported: d/dx " + f; return num(0); }
        return bin(Node::Mul, std::move(d), go(u));  // · u'  (chain rule)
    }
};

// ── Serialization (AST → canonical ASCII, minimal parentheses) ──────────────
std::string fmtNum(double v) {
    if (v == 0.0) return "0";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}
int prec(const Node& n) {
    switch (n.kind) {
        case Node::Add: case Node::Sub: return 1;
        case Node::Mul: case Node::Div: return 2;
        case Node::Neg:                 return 2;
        case Node::Pow:                 return 3;
        default:                        return 4;  // atoms, calls
    }
}
void emit(const Node& n, std::string& o, int ctx) {
    bool numNeg = (n.kind == Node::Num && n.num < 0);
    bool paren  = prec(n) < ctx || (numNeg && ctx >= 2);
    if (paren) o += '(';
    switch (n.kind) {
        case Node::Num:                   o += fmtNum(n.num); break;
        case Node::Const: case Node::Var: o += n.name;        break;
        case Node::Neg:  o += '-'; emit(*n.kids[0], o, 2); break;
        case Node::Add:  emit(*n.kids[0], o, 1); o += '+'; emit(*n.kids[1], o, 1); break;
        case Node::Sub:  emit(*n.kids[0], o, 1); o += '-'; emit(*n.kids[1], o, 2); break;
        case Node::Mul:  emit(*n.kids[0], o, 2); o += '*'; emit(*n.kids[1], o, 2); break;
        case Node::Div:  emit(*n.kids[0], o, 2); o += '/'; emit(*n.kids[1], o, 3); break;
        case Node::Pow:  emit(*n.kids[0], o, 4); o += '^'; emit(*n.kids[1], o, 3); break;
        case Node::Call: o += n.name; o += '('; emit(*n.kids[0], o, 0); o += ')'; break;
    }
    if (paren) o += ')';
}
std::string serialize(const Node& n) { std::string o; emit(n, o, 0); return o; }

NodePtr parse(const std::string& expr, std::string& err) {
    std::vector<mathx::Token> toks;
    if (!mathx::tokenize(expr, toks)) { err = "Syntax Error"; return nullptr; }
    mathx::ParseResult pr = mathx::parse(toks);
    if (!pr.ok || !pr.root)           { err = "Syntax Error"; return nullptr; }
    return std::move(pr.root);
}

}  // namespace

Reply SymbolicEngine::evaluate(const Request& req, const std::atomic<bool>& cancel) {
    Reply rep;
    std::string err;
    NodePtr root = parse(req.expr, err);
    if (!root) { rep.error = err; return rep; }
    if (cancel.load()) { rep.error = "Interrupted"; return rep; }

    switch (req.op) {
        case Op::Numeric: {
            mathx::NumericEvaluator ev;
            mathx::EvalContext ctx;
            ctx.degrees = req.degrees;
            mathx::EvalResult r = ev.eval(*root, ctx);
            if (!r.ok) { rep.error = r.error; return rep; }
            rep.ok = true; rep.text = fmtNum(r.value);
            return rep;
        }
        case Op::Simplify: {
            NodePtr s = simplify(*root);
            rep.ok = true; rep.text = serialize(*s);
            return rep;
        }
        case Op::Derivative: {
            Differ d; d.x = req.var;
            NodePtr g = d.go(*root);
            if (!d.ok)         { rep.error = d.err;        return rep; }
            if (cancel.load()) { rep.error = "Interrupted"; return rep; }
            NodePtr s = simplify(*g);
            rep.ok = true; rep.text = serialize(*s);
            return rep;
        }
        case Op::Integral:
            // Honest gap: the dyno doesn't integrate. Eigenmath/Giac will.
            rep.error = "∫ needs the CAS engine (Giac)";
            return rep;
    }
    rep.error = "Unsupported";
    return rep;
}

}  // namespace cas
