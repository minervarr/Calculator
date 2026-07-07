// mathlayout.cc — builders translating the input editor tree and the CAS result
// AST into the shared mathbox IR (mathbox.hh), plus the fit/draw entry points.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// ALL typography (TeX spacing/styles, MATH-table fractions/radicals/scripts)
// lives in mathbox; this file only decides WHAT to lay out: the editor builder
// emits carets, empty-slot placeholders, cycled parens and ghost closers; the
// AST builder derives delimiters from precedence, detects ^(1/2) radicals and
// tags negative numbers. So input and output share one geometry by construction.
#include "mathlayout.hh"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "canvas.hh"
#include "mathbox.hh"
#include "msdf.hh"
#include "calc/editor.hh"
#include "math/ast.hh"

namespace mathlayout {
namespace {

namespace mb = mathbox;
using mb::MathClass;

// ── Canonical-atom mapping (calculator-specific vocabulary) ──────────────────

// Canonical ASCII atom → pretty math glyph for display (the core keeps the ASCII).
// Mirrors Calc::prettify so the live input matches the result typography.
std::string prettyAtom(const std::string& s) {
    if (s == "*")  return "\xE2\x8B\x85";  // · (centered dot — our explicit product)
    if (s == "/")  return "\xC3\xB7";      // ÷
    if (s == "-")  return "\xE2\x88\x92";  // −
    if (s == "pi") return "\xCF\x80";      // π
    return s;
}

// Codepoint for a canonical atom that should render as one math-face glyph
// (math-italic variables, axis-placed operators, π), else 0 — digits,
// multi-letter names (sin, log) and parentheses stay upright Roman text.
uint32_t atomMathCp(const std::string& s) {
    if (s.size() == 1) {
        char ch = s[0];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) return mb::mathItalicCp(ch);
        switch (ch) {
            case '+': return 0x002B;
            case '-': return 0x2212;
            case '*': return 0x22C5;   // ·
            case '/': return 0x00F7;   // ÷
            case '=': return 0x003D;
        }
    }
    if (s == "pi") return 0x03C0;       // π
    return 0;
}
uint32_t atomMathKey(Canvas& c, const std::string& s) {
    const MsdfFont* mf = c.msdfFont();
    if (!mf || !mf->hasMath()) return 0;
    uint32_t cp = atomMathCp(s);
    return cp ? mf->mathKey(cp) : 0;
}

// TeX atom class of a canonical atom (drives the inter-atom glue in mathbox).
MathClass atomClass(const std::string& s) {
    if (s.size() == 1) {
        switch (s[0]) {
            case '+': case '-': case '*': case '/': return MathClass::Bin;
            case '=':                               return MathClass::Rel;
            case '(':                               return MathClass::Open;
            case ')':                               return MathClass::Close;
            case ',':                               return MathClass::Punct;
        }
    }
    return MathClass::Ord;
}

// IR leaf for a canonical atom: math-face glyph when the atlas has one, else
// upright text of the prettified form.
mb::NodePtr atomNode(Canvas& c, const std::string& s) {
    uint32_t k = atomMathKey(c, s);
    if (k) return mb::mkGlyph(k, atomClass(s));
    return mb::mkText(prettyAtom(s), atomClass(s));
}

// IR leaf for a math symbol by codepoint with a UTF-8 text fallback.
mb::NodePtr symNode(Canvas& c, uint32_t cp, const char* utf8, MathClass cl) {
    const MsdfFont* mf = c.msdfFont();
    uint32_t k = (mf && mf->hasMath()) ? mf->mathKey(cp) : 0;
    return k ? mb::mkGlyph(k, cl) : mb::mkText(utf8, cl);
}

// ── Editor tree → IR ─────────────────────────────────────────────────────────

using calcedit::Row;
using EdNode = calcedit::Node;

// A row item that opens / closes a delimiter. Opens are the standalone "(" and
// the function tokens "sin(" / "cos(" / … (they end in "("); close is ")".
bool opensParen(const EdNode& n) {
    return n.kind == EdNode::Atom && !n.text.empty() && n.text.back() == '(';
}
bool closesParen(const EdNode& n) {
    return n.kind == EdNode::Atom && n.text == ")";
}
// Assign every delimiter ATOM in `row` an INSIDE-OUT cycling level (−1 = not a
// delimiter): a pair wrapping content of depth d is itself level d, so the
// innermost pair is level 0. Unmatched opens (still being typed) get levels
// too, pushed onto `unclosed` innermost-first — those drive the ghost closers.
void parenLevels(const Row& row, std::vector<int>& level, std::vector<int>& unclosed,
                 std::vector<int>& match) {
    int n = static_cast<int>(row.items.size());
    level.assign(n, -1);
    match.assign(n, -1);                     // matched partner index (both ways)
    struct Open { int idx, depth, maxDepth; };
    std::vector<Open> stack;
    int depth = 0;
    for (int i = 0; i < n; ++i) {
        const EdNode& it = *row.items[i];
        if (opensParen(it)) {
            ++depth;
            for (auto& o : stack) o.maxDepth = std::max(o.maxDepth, depth);
            stack.push_back({i, depth, depth});
        } else if (closesParen(it)) {
            if (!stack.empty()) {
                Open o = stack.back(); stack.pop_back();
                int lv = o.maxDepth - o.depth;
                level[o.idx] = lv;
                level[i]     = lv;
                match[o.idx] = i;
                match[i]     = o.idx;
            }
            if (depth > 0) --depth;
        }
    }
    for (int s = static_cast<int>(stack.size()) - 1; s >= 0; --s) {   // innermost first
        int lv = stack[s].maxDepth - stack[s].depth;
        level[stack[s].idx] = lv;
        unclosed.push_back(lv);
    }
}

// Can this IR node serve as the base of a superscript? Mirrors the editor's
// "previous token ends a value" rule (digits/letters/close-paren/templates).
bool isScriptBase(const mb::Node& n) {
    switch (n.kind) {
        case mb::Node::Frac: case mb::Node::Radical: case mb::Node::Script:
        case mb::Node::Delim: case mb::Node::Placeholder:
            return true;
        case mb::Node::Text: case mb::Node::Glyph:
            return n.cls == MathClass::Ord || n.cls == MathClass::Close;
        default:
            return false;
    }
}

// Build one editor Row. The editor's Power node has NO base child (it attaches
// to the preceding sibling), so the builder resolves it structurally: pop the
// last emitted item and make it the Script's base — a dashed Placeholder when
// there is nothing to raise (base backspaced away). `unclosedOut`, when given
// (root row only), receives the levels of still-open parens for ghost closers.
mb::NodePtr buildEdRow(Canvas& c, const Row& row, const calcedit::Cursor& cur,
                       std::vector<int>* unclosedOut = nullptr) {
    auto hb = mb::mk(mb::Node::HBox);
    bool here = (cur.row == &row);
    if (row.items.empty()) {                    // empty slot → dashed placeholder
        if (here) hb->kids.push_back(mb::mk(mb::Node::Caret));
        hb->kids.push_back(mb::mk(mb::Node::Placeholder));
        return hb;
    }
    std::vector<int> level, unclosed, match;
    parenLevels(row, level, unclosed, match);
    if (unclosedOut) *unclosedOut = unclosed;
    // MATCHED paren pairs become real Delim nodes so they STRETCH around tall
    // content (fractions inside sin(…)) exactly like the results tape. `tgt` is
    // the HBox currently receiving items — inside an open pair it's the Delim's
    // inner row. Unmatched (still-typing) parens keep the flat cycled-atom form
    // + ghost closers.
    mb::Node* tgt = hb.get();
    std::vector<mb::Node*> restore;
    std::vector<int>       closeAt;
    for (int i = 0; i < static_cast<int>(row.items.size()); ++i) {
        if (here && cur.index == i) tgt->kids.push_back(mb::mk(mb::Node::Caret));
        const EdNode& it = *row.items[i];
        switch (it.kind) {
            case EdNode::Atom:
                if (level[i] >= 0 && opensParen(it)) {
                    // Every open becomes a stretchy Delim. A matched pair pops
                    // back out at its ")"; an UNMATCHED one wraps the rest of the
                    // row and draws its closer dim — the ghost, now stretching
                    // with the content instead of staying 1em.
                    std::string name = it.text.substr(0, it.text.size() - 1);
                    if (!name.empty())          // sin( → upright "sin" + stretchy (
                        tgt->kids.push_back(mb::mkText(name, MathClass::Op));
                    auto d = mb::mk(mb::Node::Delim);
                    d->level = level[i];
                    d->cls = MathClass::Inner;
                    d->ghostClose = (match[i] < 0);
                    d->kids.push_back(mb::mk(mb::Node::HBox));
                    mb::Node* inner = d->kids[0].get();
                    tgt->kids.push_back(std::move(d));
                    if (match[i] >= 0) {        // only matched pairs ever pop
                        restore.push_back(tgt);
                        closeAt.push_back(match[i]);
                    }
                    tgt = inner;
                } else if (level[i] >= 0 && closesParen(it) && match[i] >= 0) {
                    tgt = restore.back();       // the matching close: pop back out
                    restore.pop_back();
                    closeAt.pop_back();
                } else {                        // unmatched ")" and plain atoms
                    tgt->kids.push_back(atomNode(c, it.text));
                }
                break;
            case EdNode::Frac: {
                auto f = mb::mk(mb::Node::Frac);
                f->cls = MathClass::Inner;
                f->kids.push_back(buildEdRow(c, it.slots[0], cur));
                f->kids.push_back(buildEdRow(c, it.slots[1], cur));
                tgt->kids.push_back(std::move(f));
                break;
            }
            case EdNode::Sqrt: {
                auto r = mb::mk(mb::Node::Radical);
                r->kids.push_back(buildEdRow(c, it.slots[0], cur));
                tgt->kids.push_back(std::move(r));
                break;
            }
            case EdNode::NthRoot: {
                auto r = mb::mk(mb::Node::Radical);
                r->kids.push_back(buildEdRow(c, it.slots[1], cur));   // radicand
                r->kids.push_back(buildEdRow(c, it.slots[0], cur));   // degree
                tgt->kids.push_back(std::move(r));
                break;
            }
            case EdNode::Power: {
                bool caretBetween = false;      // cursor sat between base and ^
                if (!tgt->kids.empty() && tgt->kids.back()->kind == mb::Node::Caret) {
                    caretBetween = true;
                    tgt->kids.pop_back();
                }
                mb::NodePtr base;
                if (!tgt->kids.empty() && isScriptBase(*tgt->kids.back())) {
                    base = std::move(tgt->kids.back());
                    tgt->kids.pop_back();
                } else {                        // orphan: dashed placeholder base
                    if (caretBetween) {
                        tgt->kids.push_back(mb::mk(mb::Node::Caret));
                        caretBetween = false;
                    }
                    base = mb::mk(mb::Node::Placeholder);
                }
                auto sc = mb::mk(mb::Node::Script);
                sc->cls = base->cls;            // script inherits its nucleus class
                sc->scriptCaret = caretBetween;
                sc->kids.push_back(std::move(base));
                sc->kids.push_back(buildEdRow(c, it.slots[0], cur));
                tgt->kids.push_back(std::move(sc));
                break;
            }
        }
    }
    if (here && cur.index == static_cast<int>(row.items.size()))
        tgt->kids.push_back(mb::mk(mb::Node::Caret));  // may sit inside a ghost delim
    return hb;
}

// ── CAS result AST → IR ──────────────────────────────────────────────────────

using AstN = mathx::Node;

std::string numText(double v) {
    if (v == 0.0) return "0";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.10g", v);
    return buf;
}

int prec(const AstN& n) {
    switch (n.kind) {
        case AstN::Add: case AstN::Sub:                 return 1;
        case AstN::Mul: case AstN::Div: case AstN::Neg: return 2;
        case AstN::Pow:                                 return 3;
        default:                                        return 4;
    }
}
// √ is written by Eigenmath as ^(1/2); detect that to render a radical.
bool isHalf(const AstN& n) {
    if (n.kind == AstN::Num) return n.num == 0.5;
    return n.kind == AstN::Div &&
           n.kids[0]->kind == AstN::Num && n.kids[0]->num == 1.0 &&
           n.kids[1]->kind == AstN::Num && n.kids[1]->num == 2.0;
}

// Inside-out delimiter levels: `delimDepth(n)` = the deepest delimiter nesting
// WITHIN n's rendering, so the pair a node draws sits at level = depth of what
// it wraps and the innermost pair is level 0 (see mathbox delimiter cycling).
int delimDepth(const AstN& n);
int wrapDepth(const AstN& n, int ctx) {
    return (prec(n) < ctx ? 1 : 0) + delimDepth(n);
}
int delimDepth(const AstN& n) {
    switch (n.kind) {
        case AstN::Num: case AstN::Const: case AstN::Var: return 0;
        case AstN::Neg: return wrapDepth(*n.kids[0], 2);
        case AstN::Add: case AstN::Sub:
            return std::max(wrapDepth(*n.kids[0], 1), wrapDepth(*n.kids[1], 2));
        case AstN::Mul:
            return std::max(wrapDepth(*n.kids[0], 2), wrapDepth(*n.kids[1], 2));
        case AstN::Div:   // fraction bar — no parens
            return std::max(delimDepth(*n.kids[0]), delimDepth(*n.kids[1]));
        case AstN::Pow:
            if (isHalf(*n.kids[1])) return delimDepth(*n.kids[0]);   // radical — no parens
            return std::max(wrapDepth(*n.kids[0], 4), delimDepth(*n.kids[1]));
        case AstN::Call: return 1 + delimDepth(*n.kids[0]);          // always wraps the arg
    }
    return 0;
}

mb::NodePtr buildAst(Canvas& c, const AstN& n);

// Wrap in cycled delimiters when precedence demands it (a wrapped subformula
// is TeX class Inner — that's what gives \left(…\right) its breathing room).
mb::NodePtr wrapAst(Canvas& c, const AstN& n, int ctx) {
    auto inner = buildAst(c, n);
    if (prec(n) < ctx) {
        auto d = mb::mk(mb::Node::Delim);
        d->level = delimDepth(n);
        d->cls = MathClass::Inner;
        d->kids.push_back(std::move(inner));
        return d;
    }
    return inner;
}

mb::NodePtr buildAst(Canvas& c, const AstN& n) {
    const MsdfFont* mf = c.msdfFont();
    switch (n.kind) {
        case AstN::Num: {
            auto t = mb::mkText(numText(n.num));
            t->negative = n.num < 0.0;          // tinted red only on the tape
            return t;
        }
        case AstN::Const: {
            uint32_t k = 0;
            if (mf && mf->hasMath()) {
                if (n.name == "pi")     k = mf->mathKey(0x03C0);              // upright π
                else if (n.name == "e") k = mf->mathKey(mb::mathItalicCp('e'));
            }
            if (k) return mb::mkGlyph(k);
            return mb::mkText(n.name == "pi" ? "\xCF\x80" : n.name);
        }
        case AstN::Var: {
            uint32_t k = 0;
            if (mf && mf->hasMath() && n.name.size() == 1) {
                uint32_t cp = mb::mathItalicCp(n.name[0]);
                if (cp) k = mf->mathKey(cp);
            }
            if (k) return mb::mkGlyph(k);
            return mb::mkText(n.name);
        }
        case AstN::Neg: {                       // Bin at list start demotes → tight sign
            auto hb = mb::mk(mb::Node::HBox);
            auto m = symNode(c, 0x2212, "\xE2\x88\x92", MathClass::Bin);
            if (n.kids[0]->kind == AstN::Num) { // −3 → a negative number, in red
                m->negative = true;
                auto t = mb::mkText(numText(n.kids[0]->num));
                t->negative = true;
                hb->kids.push_back(std::move(m));
                hb->kids.push_back(std::move(t));
            } else {
                hb->kids.push_back(std::move(m));
                hb->kids.push_back(wrapAst(c, *n.kids[0], 2));
            }
            return hb;
        }
        case AstN::Add: case AstN::Sub: {
            auto hb = mb::mk(mb::Node::HBox);
            hb->kids.push_back(wrapAst(c, *n.kids[0], 1));
            hb->kids.push_back(n.kind == AstN::Add
                                   ? symNode(c, 0x002B, "+", MathClass::Bin)
                                   : symNode(c, 0x2212, "\xE2\x88\x92", MathClass::Bin));
            hb->kids.push_back(wrapAst(c, *n.kids[1], 2));
            return hb;
        }
        case AstN::Mul: {   // explicit · — consistent with the input, and it
                            // disambiguates "3·x^n" from "(3·x)^n".
            auto hb = mb::mk(mb::Node::HBox);
            hb->kids.push_back(wrapAst(c, *n.kids[0], 2));
            hb->kids.push_back(symNode(c, 0x22C5, "\xE2\x8B\x85", MathClass::Bin));
            hb->kids.push_back(wrapAst(c, *n.kids[1], 2));
            return hb;
        }
        case AstN::Div: {
            auto f = mb::mk(mb::Node::Frac);
            f->cls = MathClass::Inner;
            f->kids.push_back(buildAst(c, *n.kids[0]));
            f->kids.push_back(buildAst(c, *n.kids[1]));
            return f;
        }
        case AstN::Pow: {
            if (isHalf(*n.kids[1])) {
                auto r = mb::mk(mb::Node::Radical);
                r->kids.push_back(buildAst(c, *n.kids[0]));
                return r;
            }
            auto sc = mb::mk(mb::Node::Script);
            auto base = wrapAst(c, *n.kids[0], 4);
            sc->cls = base->cls;
            sc->kids.push_back(std::move(base));
            sc->kids.push_back(buildAst(c, *n.kids[1]));
            return sc;
        }
        case AstN::Call: {
            auto hb = mb::mk(mb::Node::HBox);
            hb->kids.push_back(mb::mkText(n.name, MathClass::Op));
            auto d = mb::mk(mb::Node::Delim);
            d->level = delimDepth(*n.kids[0]);
            d->cls = MathClass::Inner;
            d->kids.push_back(buildAst(c, *n.kids[0]));
            hb->kids.push_back(std::move(d));
            return hb;
        }
    }
    return mb::mkText("?");
}

}  // namespace

// ── Entry points ─────────────────────────────────────────────────────────────

void drawEditor(Canvas& c, const calcedit::Editor& ed, const Rect& panel) {
    // Unclosed parens render as Delim nodes with a DIM ghost closer built by
    // buildEdRow itself — so the ghost stretches with the content exactly like
    // a typed closer would.
    mb::NodePtr root = buildEdRow(c, ed.root(), ed.cursor());

    const float base   = panel.h * 0.46f;
    const float availW = panel.w - panel.h * 0.30f;
    const float availH = panel.h * 0.92f;

    float size = base;
    mb::Box b = mb::layout(c, *root, size);
    if (b.w > availW && b.w > 0.0f) size = std::min(size, base * availW / b.w);
    float H = b.above + b.below;
    if (H > availH && H > 0.0f) size = std::min(size, size * availH / H);
    size = std::max(size, base * 0.40f);   // floor; taller content clips to panel

    b = mb::layout(c, *root, size);
    float rightX = panel.x + panel.w - panel.h * 0.18f;
    float xStart = rightX - b.w;
    float yAxis  = panel.y + (panel.h - (b.above + b.below)) * 0.5f + b.above;
    mb::Style st;
    st.caret = true;
    mb::draw(c, *root, xStart, yAxis, st);
}

AstFit fitAst(Canvas& c, const mathx::Node& root, float maxSize, float maxWidth) {
    mb::NodePtr t = buildAst(c, root);
    float size = maxSize;
    mb::Box b = mb::layout(c, *t, size);
    if (b.w > maxWidth && b.w > 0.0f) size = std::min(size, size * maxWidth / b.w);
    // Only a GENTLE shrink (≤15%) to swallow near-misses; anything wider stays at
    // this size and OVERFLOWS so the tape can scroll it. A low floor (e.g. 0.65)
    // crushed long equations to fit exactly (width==maxW → overflow 0 → scroll
    // never triggered).
    size = std::max(size, maxSize * 0.85f);
    b = mb::layout(c, *t, size);
    return {size, b.w, b.above, b.below};
}

void drawAstAt(Canvas& c, const mathx::Node& root, float rightX, float yAxis, float size,
               const Color* ink) {
    mb::NodePtr t = buildAst(c, root);
    mb::Box b = mb::layout(c, *t, size);
    mb::Style st;
    if (ink) st.ink = *ink;         // uniform tint (tape's gray source echo)
    else     st.negativeRed = true; // result: negatives in red
    mb::draw(c, *t, rightX - b.w, yAxis, st);   // right-aligned at rightX
}

namespace {

// Math-italic leaf for a variable letter (Text fallback without a math font).
mb::NodePtr italicVarNode(Canvas& c, char var) {
    const MsdfFont* mf = c.msdfFont();
    uint32_t k = 0;
    if (mf && mf->hasMath()) {
        uint32_t cp = mb::mathItalicCp(var);
        if (cp) k = mf->mathKey(cp);
    }
    return k ? mb::mkGlyph(k) : mb::mkText(std::string(1, var));
}

// The full equation-label IR: italic lhs (with optional subscript index — the
// automatic 𝑦₁/𝑦₂/𝑟₁ names), '=' with relation spacing, then the expression(s).
mb::NodePtr buildEqLabelIR(Canvas& c, const char* lhs, int subIndex,
                           const mathx::Node* exprX, const mathx::Node* exprY) {
    auto hb = mb::mk(mb::Node::HBox);
    for (const char* p = lhs; *p; ++p) hb->kids.push_back(italicVarNode(c, *p));
    if (subIndex > 0 && !hb->kids.empty()) {      // y → y_i
        auto sc = mb::mk(mb::Node::Script);
        sc->sub = true;
        sc->cls = hb->kids.back()->cls;
        sc->kids.push_back(std::move(hb->kids.back()));
        hb->kids.pop_back();
        sc->kids.push_back(mb::mkText(std::to_string(subIndex)));
        hb->kids.push_back(std::move(sc));
    }
    hb->kids.push_back(symNode(c, 0x003D, "=", MathClass::Rel));
    auto rhs = [&](const mathx::Node* ast) -> mb::NodePtr {
        return ast ? buildAst(c, *ast) : mb::mk(mb::Node::Placeholder);
    };
    if (exprY) {                                  // parametric: ( x-expr , y-expr )
        hb->kids.push_back(mb::mkText("(", MathClass::Open));
        hb->kids.push_back(rhs(exprX));
        hb->kids.push_back(mb::mkText(",", MathClass::Punct));
        hb->kids.push_back(rhs(exprY));
        hb->kids.push_back(mb::mkText(")", MathClass::Close));
    } else {
        hb->kids.push_back(rhs(exprX));
    }
    return hb;
}

// Size at which `hb` fits `panel` (nominal panel.h·0.62, shrunk for both
// dimensions, floored at 0.45× — wider content clips).
float fitEqSize(Canvas& c, mb::Node& hb, const Rect& panel) {
    float size = panel.h * 0.62f;
    mb::Box b = mb::layout(c, hb, size);
    float k = 1.0f;
    if (b.w > panel.w && b.w > 0.0f) k = std::min(k, panel.w / b.w);
    float H = b.above + b.below;
    if (H > panel.h && H > 0.0f) k = std::min(k, panel.h / H);
    return size * std::max(k, 0.45f);
}

}  // namespace

float fitEqLabelSize(Canvas& c, const char* lhs, int subIndex, const mathx::Node* exprX,
                     const mathx::Node* exprY, const Rect& panel) {
    mb::NodePtr hb = buildEqLabelIR(c, lhs, subIndex, exprX, exprY);
    return fitEqSize(c, *hb, panel);
}

void drawEqLabel(Canvas& c, const char* lhs, int subIndex, const mathx::Node* exprX,
                 const mathx::Node* exprY, const Rect& panel, const Color& ink,
                 float size) {
    mb::NodePtr hb = buildEqLabelIR(c, lhs, subIndex, exprX, exprY);
    if (size <= 0.0f) size = fitEqSize(c, *hb, panel);
    mb::Box b = mb::layout(c, *hb, size);
    float yAxis = panel.y + (panel.h - (b.above + b.below)) * 0.5f + b.above;
    mb::Style st;
    st.ink = ink;
    mb::draw(c, *hb, panel.x, yAxis, st);
}

void drawVarName(Canvas& c, char var, int subIndex, const Rect& r, const Color& ink) {
    mb::NodePtr root = italicVarNode(c, var);
    if (subIndex > 0) {
        auto sc = mb::mk(mb::Node::Script);
        sc->sub = true;
        sc->kids.push_back(std::move(root));
        sc->kids.push_back(mb::mkText(std::to_string(subIndex)));
        root = std::move(sc);
    }
    float size = r.h * 0.55f;
    mb::Box b = mb::layout(c, *root, size);
    float x = r.x + (r.w - b.w) * 0.5f;
    float yAxis = r.y + (r.h - (b.above + b.below)) * 0.5f + b.above;
    mb::Style st;
    st.ink = ink;
    mb::draw(c, *root, x, yAxis, st);
}

void drawGhostLhs(Canvas& c, const char* lhs, int subIndex, const Rect& r,
                  const Color& ink) {
    auto hb = mb::mk(mb::Node::HBox);
    for (const char* p = lhs; *p; ++p) hb->kids.push_back(italicVarNode(c, *p));
    if (subIndex > 0 && !hb->kids.empty()) {      // y → y_i
        auto sc = mb::mk(mb::Node::Script);
        sc->sub = true;
        sc->cls = hb->kids.back()->cls;
        sc->kids.push_back(std::move(hb->kids.back()));
        hb->kids.pop_back();
        sc->kids.push_back(mb::mkText(std::to_string(subIndex)));
        hb->kids.push_back(std::move(sc));
    }
    hb->kids.push_back(symNode(c, 0x003D, "=", MathClass::Rel));
    float size = r.h * 0.42f;
    mb::Box b = mb::layout(c, *hb, size);
    if (b.w > r.w && b.w > 0.0f) { size *= r.w / b.w; b = mb::layout(c, *hb, size); }
    float yAxis = r.y + (r.h - (b.above + b.below)) * 0.5f + b.above;
    mb::Style st;
    st.ink = ink;
    mb::draw(c, *hb, r.x, yAxis, st);
}

// ── LaTeX-style key icons ─────────────────────────────────────────────────────
// One drawing vocabulary for keypad templates: the same math-italic glyphs, MATH
// surd and bar geometry the editor/tape use, plus dashed placeholder squares in
// the editor's empty-slot style — so a key looks like what it inserts.
namespace {

// Dashed placeholder square tuned for key-icon scale (chunkier dashes than the
// editor's dashedRect so it reads at button size), in the key's palette.
void dashedBox(Canvas& c, float x, float y, float w, float h, Color col) {
    float dash = std::max(2.0f, h * 0.18f), step = dash * 2.0f;
    float t = std::max(1.0f, h * 0.07f);
    for (float px = x; px < x + w; px += step) {
        float ww = std::min(dash, x + w - px);
        c.rect(px, y, ww, t, col);
        c.rect(px, y + h - t, ww, t, col);
    }
    for (float py = y; py < y + h; py += step) {
        float hh = std::min(dash, y + h - py);
        c.rect(x, py, t, hh, col);
        c.rect(x + w - t, py, t, hh, col);
    }
}

// Math-italic glyph key for a variable letter, or 0 when no MATH font is loaded.
uint32_t italicVarKey(Canvas& c, char ch) {
    const MsdfFont* mf = c.msdfFont();
    if (!mf || !mf->hasMath()) return 0;
    uint32_t cp = mb::mathItalicCp(ch);
    return cp ? mf->mathKey(cp) : 0;
}

}  // namespace

void drawKeyIcon(Canvas& c, KeyIcon icon, const Rect& r, const Color& fg, char ch) {
    const MsdfFont* mf = c.msdfFont();
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    // Placeholder squares slightly dimmer than the key's main ink (like the
    // editor's dim empty slots) so the structure reads, not the box.
    Color ph = {fg.r * 0.62f, fg.g * 0.62f, fg.b * 0.62f, fg.a};

    // No MATH font → the legacy ASCII labels (same fallback policy as the tape).
    if (!(mf && mf->hasMath())) {
        const char* s;
        char vb[2] = {ch, 0};
        switch (icon) {
            case KeyIcon::Var:        s = vb;           break;
            case KeyIcon::Frac:       s = "a/b";        break;
            case KeyIcon::Sqrt:       s = "\xE2\x88\x9A";   break;
            case KeyIcon::NthRoot:    s = "n\xE2\x88\x9A";  break;
            case KeyIcon::Power:      s = "x^n";        break;
            case KeyIcon::Integral:   s = "Int";        break;
            case KeyIcon::Derivative: s = "d/dx";       break;
            default:                  s = "?";          break;
        }
        float size = r.h * 0.32f;
        c.text(s, cx - c.textWidth(s, size) * 0.5f, cy - size * 0.5f + size, size, fg);
        return;
    }

    switch (icon) {
        case KeyIcon::Var: {                       // math-italic variable, centred
            uint32_t k = italicVarKey(c, ch);
            float size = r.h * 0.42f;
            float w = mf->advanceKey(k, size);
            // Centre the x-height ink: baseline sits ~half an x-height below centre.
            c.mathGlyph(k, cx - w * 0.5f, cy + size * 0.23f, size, fg);
            return;
        }
        case KeyIcon::Frac: {                      // □ over bar over □
            float s = r.h * 0.34f;
            float q = s * 0.52f;
            float t = std::max(1.5f, s * 0.09f);
            float gap = s * 0.16f;
            float barW = q * 1.7f;
            c.rect(cx - barW * 0.5f, cy - t * 0.5f, barW, t, fg);
            dashedBox(c, cx - q * 0.5f, cy - t * 0.5f - gap - q, q, q, ph);
            dashedBox(c, cx - q * 0.5f, cy + t * 0.5f + gap, q, q, ph);
            return;
        }
        case KeyIcon::Sqrt:
        case KeyIcon::NthRoot: {                   // real MATH surd wrapping a □
            float s = r.h * 0.34f;
            float q = s * 0.62f;
            mb::Box rr{q, q * 0.5f, q * 0.5f};     // radicand square centred on axis
            mb::Box b = mb::radicalMeasure(c, rr, s);
            float is = s * 0.55f;                  // degree size (NthRoot)
            uint32_t nk = icon == KeyIcon::NthRoot ? italicVarKey(c, 'n') : 0;
            float base = nk ? mf->advanceKey(nk, is) * 0.70f : 0.0f;
            float yAxis = cy + (b.above - b.below) * 0.5f;
            float x0 = cx - (base + b.w) * 0.5f;
            float radX = mb::radicalDraw(c, rr, x0 + base, yAxis, s, fg);
            dashedBox(c, radX, yAxis - q * 0.5f, q, q, ph);
            if (nk) c.mathGlyph(nk, x0, yAxis - q * 0.5f, is, fg);  // degree, above-left
            return;
        }
        case KeyIcon::Power: {                     // italic var with raised □
            uint32_t k = italicVarKey(c, ch);
            float size = r.h * 0.40f;
            float bw = mf->advanceKey(k, size);
            float q = size * 0.42f;
            float supGap = size * 0.06f;
            float x0 = cx - (bw + supGap + q) * 0.5f;
            float baseline = cy + size * 0.28f;
            c.mathGlyph(k, x0, baseline, size, fg);
            dashedBox(c, x0 + bw + supGap, baseline - size * 0.55f - q, q, q, ph);
            return;
        }
        case KeyIcon::Integral: {                  // the math face's real ∫
            uint32_t k = mf->mathKey(0x222B);
            const MsdfGlyph* g = k ? mf->glyphByKey(k) : nullptr;
            if (!g) {
                mb::axisText(c, "Int", cx - c.textWidth("Int", r.h * 0.32f) * 0.5f,
                             cy, r.h * 0.32f, fg);
                return;
            }
            // Fit the tall glyph's ink to ~60% of the key height, centre by ink
            // extents (the pads in plane[] cancel out of the midpoint).
            float pad = mf->glyphPadEm();
            float inkHEm = (g->planeB - g->planeT) - 2.0f * pad;
            float size = (r.h * 0.60f) / std::max(inkHEm, 0.1f);
            float baseline = cy - (g->planeT + g->planeB) * 0.5f * size;
            float penX = cx - (g->planeL + g->planeR) * 0.5f * size;
            c.mathGlyph(k, penX, baseline, size, fg);
            return;
        }
        case KeyIcon::Derivative: {                // d over dx, a real fraction
            uint32_t dk = italicVarKey(c, 'd');
            uint32_t xk = italicVarKey(c, ch);
            float s = r.h * 0.30f;
            float wd = mf->advanceKey(dk, s);
            float wx = mf->advanceKey(xk, s);
            float t = std::max(1.5f, s * 0.09f);
            float gap = s * 0.14f;
            float wDen = wd + wx;
            float barW = std::max(wd, wDen) * 1.15f;
            c.rect(cx - barW * 0.5f, cy - t * 0.5f, barW, t, fg);
            c.mathGlyph(dk, cx - wd * 0.5f, cy - t * 0.5f - gap, s, fg);           // numerator d
            float denBase = cy + t * 0.5f + gap + s * 0.70f;                        // d's ascender below bar
            c.mathGlyph(dk, cx - wDen * 0.5f, denBase, s, fg);                      // denominator d…
            c.mathGlyph(xk, cx - wDen * 0.5f + wd, denBase, s, fg);                 // …x
            return;
        }
    }
}

}  // namespace mathlayout
