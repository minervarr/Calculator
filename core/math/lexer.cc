// lexer.cc — tokenizer for the calculator math core.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "lexer.hh"

#include <cctype>
#include <cstdlib>

namespace mathx {

bool isFunc(const std::string& s) {
    return s == "sin" || s == "cos" || s == "tan" ||
           s == "ln"  || s == "log" || s == "sqrt";
}

bool isConst(const std::string& s) { return s == "pi" || s == "e"; }

namespace {
// True when `t` is the end of a value (so a following value implies '*').
// Constants and variables end a value; a function name does NOT (it precedes a
// '(' call, e.g. sin(x) must not become sin*(x)).
bool endsValue(const Token& t) {
    return t.kind == Tok::Num || t.kind == Tok::RParen ||
           (t.kind == Tok::Ident && !isFunc(t.ident));
}
// True when `t` starts a value (so a preceding value implies '*').
bool startsValue(const Token& t) {
    return t.kind == Tok::Num || t.kind == Tok::LParen || t.kind == Tok::Ident;
}
}  // namespace

bool tokenize(const std::string& src, std::vector<Token>& out, bool implicitMul) {
    std::vector<Token> raw;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        // UTF-8 operators: · (U+00B7 = C2 B7) and ⋅ (U+22C5 = E2 8B 85) are
        // multiplication; × (U+00D7 = C3 97) is the reserved cross product.
        unsigned char u0 = static_cast<unsigned char>(c);
        if (u0 == 0xC2 && i + 1 < n && (unsigned char)src[i + 1] == 0xB7) {
            raw.push_back({Tok::Star, 0.0, ""}); i += 2; continue;
        }
        if (u0 == 0xE2 && i + 2 < n && (unsigned char)src[i + 1] == 0x8B &&
            (unsigned char)src[i + 2] == 0x85) {
            raw.push_back({Tok::Star, 0.0, ""}); i += 3; continue;
        }
        if (u0 == 0xC3 && i + 1 < n && (unsigned char)src[i + 1] == 0x97) {
            raw.push_back({Tok::Cross, 0.0, ""}); i += 2; continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { i++; continue; }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const char* start = src.c_str() + i;
            char* end = nullptr;
            double v = std::strtod(start, &end);
            if (end == start) return false;  // lone '.' etc.
            i += static_cast<size_t>(end - start);
            raw.push_back({Tok::Num, v, ""});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n && std::isalpha(static_cast<unsigned char>(src[j]))) j++;
            raw.push_back({Tok::Ident, 0.0, src.substr(i, j - i)});
            i = j;
            continue;
        }
        Tok k;
        switch (c) {
            case '+': k = Tok::Plus;   break;
            case '-': k = Tok::Minus;  break;
            case '*': k = Tok::Star;   break;
            case '/': k = Tok::Slash;  break;
            case '^': k = Tok::Caret;  break;
            case '(': k = Tok::LParen; break;
            case ')': k = Tok::RParen; break;
            default:  return false;
        }
        raw.push_back({k, 0.0, ""});
        i++;
    }

    // Implicit multiplication: only when explicitly enabled (CAS-output re-parse).
    // Off by default → adjacent values like "2x"/"2 x" are left adjacent, and the
    // parser rejects them, forcing the user to write the explicit "2·x".
    out.clear();
    out.reserve(raw.size() * 2 + 1);
    for (size_t t = 0; t < raw.size(); t++) {
        if (implicitMul && t > 0 && endsValue(raw[t - 1]) && startsValue(raw[t]))
            out.push_back({Tok::Star, 0.0, ""});
        out.push_back(raw[t]);
    }
    out.push_back({Tok::End, 0.0, ""});
    return true;
}

}  // namespace mathx
