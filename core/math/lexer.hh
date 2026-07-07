// lexer.hh — tokenizer for the calculator math core.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#pragma once
#include <string>
#include <vector>

namespace mathx {

// Star = multiplication ('*' or the display dots '·'/'⋅'). Cross = the reserved
// 3-D cross product ('×') — NEVER a scalar multiply; the parser rejects it until
// vectors land, so '×' can never silently mean multiplication.
enum class Tok { Num, Plus, Minus, Star, Slash, Caret, LParen, RParen, Ident, Cross, End };

struct Token {
    Tok         kind;
    double      num = 0.0;   // valid for Num
    std::string ident;       // valid for Ident
};

bool isFunc(const std::string& s);    // sin cos tan ln log sqrt
bool isConst(const std::string& s);   // pi e

// Tokenize `src`. The multiplication operator is explicit ('*', or the UTF-8 dots
// '·' U+00B7 / '⋅' U+22C5). Implicit multiplication (e.g. 2(3), 2pi, )( ) is OFF by
// default — adjacent values are a parse error — to enforce the explicit-· rule;
// pass implicitMul=true ONLY for re-parsing CAS output (Eigenmath emits "2 x").
// Returns false on an unrecognized character.
bool tokenize(const std::string& src, std::vector<Token>& out, bool implicitMul = false);

}  // namespace mathx
