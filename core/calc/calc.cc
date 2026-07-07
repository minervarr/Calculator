// calc.cc — calculator facade bridging UI key presses and the math core.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "calc.hh"

#include <cstdio>

#include "math/lexer.hh"
#include "math/parser.hh"
#include "math/numeric_evaluator.hh"

namespace {

// Format a finite number compactly; %g switches to scientific notation for very
// large/small magnitudes, so a result never grows unbounded.
std::string formatNumber(double v) {
    if (v == 0.0) return "0";  // avoid "-0"
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return std::string(buf);
}

// Canonical text appended for a function/constant/char token. (Templates —
// frac/sqrt/nthroot/pow — are handled structurally and never reach here.)
std::string tokenText(const std::string& t) {
    if (t == "sin") return "sin(";
    if (t == "cos") return "cos(";
    if (t == "tan") return "tan(";
    if (t == "ln")  return "ln(";
    if (t == "log") return "log(";
    return t;  // digits, '.', operators, parens, "pi", "e"
}

bool isOperatorToken(const std::string& t) {
    return t == "+" || t == "-" || t == "*" || t == "/" || t == "^";
}

bool isDigitToken(const std::string& t) {
    return t.size() == 1 && t[0] >= '0' && t[0] <= '9';
}

// canonical ASCII -> pretty UTF-8 for the result display (· ÷ − √ π). Multiplication
// shows as the centered dot · (U+22C5), our single explicit product operator.
std::string prettify(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 5, "sqrt(") == 0) { o += "√("; i += 5; continue; }
        if (s.compare(i, 2, "pi")    == 0) { o += "π";  i += 2; continue; }
        char c = s[i++];
        switch (c) {
            case '*': o += "\xE2\x8B\x85"; break;  // ·
            case '/': o += "÷"; break;
            case '-': o += "−"; break;
            default:  o += c;   break;
        }
    }
    return o;
}

}  // namespace

void Calc::input(const std::string& token) {
    if (token == "C")      { clear();     return; }
    if (token == "back")   { backspace(); return; }
    if (token == "degrad") { degrees_ = !degrees_; return; }
    if (token == "=")      { evaluate();  return; }

    // Cursor navigation only applies while editing (not on a shown result).
    if (token == "left" || token == "right" || token == "up" || token == "down") {
        if (showingResult_) return;
        if (token == "left")  edit_.moveLeft();
        if (token == "right") edit_.moveRight();
        if (token == "up")    edit_.moveUp();
        if (token == "down")  edit_.moveDown();
        return;
    }

    appendToken(token);
}

void Calc::appendToken(const std::string& token) {
    if (showingResult_) {
        // Continue from the answer on an operator; otherwise start fresh.
        if (isOperatorToken(token)) {
            edit_.clear();
            for (char ch : formatNumber(ans_)) edit_.insertLiteral(std::string(1, ch));
        } else {
            edit_.clear();
        }
        showingResult_ = false;
        result_.clear();
    }

    // 2D templates.
    if (token == "frac")    { edit_.insertFraction(); return; }
    if (token == "sqrt")    { edit_.insertSqrt();     return; }
    if (token == "nthroot") { edit_.insertNthRoot();  return; }
    if (token == "pow")     { edit_.insertPower();     return; }

    // Top-level entry niceties (mirroring the old flat behavior).
    if (edit_.cursorAtRoot()) {
        if (isDigitToken(token) && edit_.isLoneZero()) {
            edit_.clear();
            edit_.insertAtom(token);   // replace a lone leading zero
            return;
        }
        if (token == "." && edit_.empty()) {
            edit_.insertAtom("0");
            edit_.insertAtom(".");     // ".5" reads nicer as "0.5"
            return;
        }
    }

    edit_.insertAtom(tokenText(token));
}

void Calc::evaluate() {
    // '=' on an already-shown result is a no-op.
    if (showingResult_) return;

    std::string work = edit_.linearize();
    if (work.empty()) return;

    // Auto-corrections: trailing operator -> append Ans; balance parentheses.
    char last = work.back();
    if (last == '+' || last == '-' || last == '*' || last == '/' || last == '^')
        work += formatNumber(ans_);
    int open = 0;
    for (char c : work) { if (c == '(') open++; else if (c == ')') open--; }
    for (; open > 0; open--) work += ')';

    std::vector<mathx::Token> toks;
    if (!mathx::tokenize(work, toks)) {
        result_ = "Syntax Error";
        showingResult_ = true;
        return;
    }
    mathx::ParseResult pr = mathx::parse(toks);
    if (!pr.ok || !pr.root) {
        result_ = "Syntax Error";
        showingResult_ = true;
        return;
    }
    mathx::NumericEvaluator evaluator;
    mathx::EvalContext ctx;
    ctx.degrees = degrees_;
    ctx.ans     = ans_;
    mathx::EvalResult r = evaluator.eval(*pr.root, ctx);
    if (!r.ok) {
        result_ = r.error;        // "Undefined" / "Math Error"
    } else {
        ans_    = r.value;
        result_ = formatNumber(r.value);
    }
    showingResult_ = true;
}

void Calc::clear() {
    edit_.clear();
    result_.clear();
    showingResult_ = false;
}

void Calc::backspace() {
    if (showingResult_) {
        // Drop the result overlay and reveal the expression for further editing.
        showingResult_ = false;
        result_.clear();
        return;
    }
    edit_.backspace();
}

std::string Calc::displayText() const {
    if (showingResult_) return prettify(result_);
    return prettify(edit_.linearize());
}

std::string Calc::preview() const {
    if (showingResult_) return "";
    std::string work = edit_.linearize();
    if (work.empty()) return "";
    int open = 0;  // balance parens so a half-typed "sin(" still previews
    for (char c : work) { if (c == '(') open++; else if (c == ')') open--; }
    for (; open > 0; open--) work += ')';

    std::vector<mathx::Token> toks;
    if (!mathx::tokenize(work, toks)) return "";
    mathx::ParseResult pr = mathx::parse(toks);
    if (!pr.ok || !pr.root) return "";
    mathx::NumericEvaluator evaluator;
    mathx::EvalContext ctx;
    ctx.degrees = degrees_;
    ctx.ans     = ans_;
    mathx::EvalResult r = evaluator.eval(*pr.root, ctx);
    return r.ok ? formatNumber(r.value) : std::string();
}

void Calc::insertAnsValue() {
    if (showingResult_) { edit_.clear(); result_.clear(); showingResult_ = false; }
    for (char ch : formatNumber(ans_)) edit_.insertLiteral(std::string(1, ch));
}
