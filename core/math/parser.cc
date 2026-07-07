// parser.cc — recursive-descent parser producing the expression AST.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Grammar (lowest to highest precedence):
//   expr  := term (('+'|'-') term)*
//   term  := power (('*'|'/') power)*
//   power := unary ('^' power)?            // right-associative
//   unary := ('-'|'+') unary | primary
//   primary := Num | Const | Func '(' expr ')' | '(' expr ')'
#include "parser.hh"

namespace mathx {
namespace {

struct Parser {
    const std::vector<Token>& t;
    size_t pos = 0;
    bool   ok  = true;

    explicit Parser(const std::vector<Token>& toks) : t(toks) {}

    const Token& cur() const { return t[pos]; }
    void advance() { if (t[pos].kind != Tok::End) pos++; }
    void fail() { ok = false; }

    NodePtr binary(Node::Kind k, NodePtr a, NodePtr b) {
        auto n = std::make_unique<Node>(k);
        n->kids.push_back(std::move(a));
        n->kids.push_back(std::move(b));
        return n;
    }

    NodePtr expr() {
        NodePtr a = term();
        while (ok && (cur().kind == Tok::Plus || cur().kind == Tok::Minus)) {
            Node::Kind k = (cur().kind == Tok::Plus) ? Node::Add : Node::Sub;
            advance();
            a = binary(k, std::move(a), term());
        }
        return a;
    }

    NodePtr term() {
        NodePtr a = power();
        while (ok && (cur().kind == Tok::Star || cur().kind == Tok::Slash)) {
            Node::Kind k = (cur().kind == Tok::Star) ? Node::Mul : Node::Div;
            advance();
            a = binary(k, std::move(a), power());
        }
        return a;
    }

    NodePtr power() {
        NodePtr base = unary();
        if (ok && cur().kind == Tok::Caret) {
            advance();
            return binary(Node::Pow, std::move(base), power());  // right-assoc
        }
        return base;
    }

    NodePtr unary() {
        if (cur().kind == Tok::Minus) {
            advance();
            auto n = std::make_unique<Node>(Node::Neg);
            n->kids.push_back(unary());
            return n;
        }
        if (cur().kind == Tok::Plus) { advance(); return unary(); }
        return primary();
    }

    NodePtr primary() {
        const Token& tk = cur();
        if (tk.kind == Tok::Num) { advance(); return makeNum(tk.num); }
        if (tk.kind == Tok::Ident) {
            std::string name = tk.ident;
            if (isConst(name)) {
                advance();
                auto n = std::make_unique<Node>(Node::Const);
                n->name = name;
                return n;
            }
            if (isFunc(name)) {
                advance();
                if (cur().kind != Tok::LParen) { fail(); return makeNum(0); }
                advance();  // consume '('
                NodePtr arg = expr();
                if (cur().kind == Tok::RParen) advance();  // tolerate missing ')'
                auto n = std::make_unique<Node>(Node::Call);
                n->name = name;
                n->kids.push_back(std::move(arg));
                return n;
            }
            // Otherwise a variable (x, t, θ, or a future user-defined name). The
            // evaluator resolves it via EvalContext; unbound → "Undefined".
            advance();
            auto n = std::make_unique<Node>(Node::Var);
            n->name = name;
            return n;
        }
        if (tk.kind == Tok::LParen) {
            advance();
            NodePtr inner = expr();
            if (cur().kind == Tok::RParen) advance();  // tolerate missing ')'
            return inner;
        }
        fail();
        return makeNum(0);
    }
};

}  // namespace

ParseResult parse(const std::vector<Token>& toks) {
    Parser p(toks);
    NodePtr root = p.expr();
    // Anything left over (e.g. a stray ')') is a structural error.
    if (p.cur().kind != Tok::End) p.ok = false;
    ParseResult r;
    r.ok   = p.ok;
    r.root = std::move(root);
    return r;
}

}  // namespace mathx
