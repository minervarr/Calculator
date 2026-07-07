// mathbox.hh — TeX-style math layout core shared by the input editor and the
// CAS results tape.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// A small box IR (HBox/Text/Glyph/Frac/Radical/Script/Delim/Placeholder/Caret)
// laid out with TeX's real rules, every parameter read from the math font's
// OpenType MATH table: the Display/Text/Script/ScriptScript style system, the
// TeXbook ch.18 inter-class spacing table (thin/med/thick muskips), fraction
// shift-up/-down + minimum gaps, and stretchy vertical constructions for the
// radical. Builders (mathlayout.cc) translate the editor tree and the CAS AST
// into this IR; per-surface differences live in `Style` (caret, negative-red)
// or in WHAT the builders emit (placeholders, ghost closers) — the geometry
// itself has exactly ONE definition here, so input and output cannot drift.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "canvas.hh"

namespace mathbox {

// Every element is a box around the shared horizontal math axis (the line
// fraction bars balance on): above/below are the extents up/down from it.
struct Box { float w = 0, above = 0, below = 0; };

// TeX styles. Order matters: >= Script means "script-size context" (glue that
// the spacing table marks suppressible is dropped, like TeX does).
enum class MathStyle : uint8_t { Display, Text, Script, ScriptScript };

// TeX atom classes driving inter-atom glue (TeXbook ch.18).
enum class MathClass : uint8_t { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

// The per-surface toggles — everything else is common typography.
struct Style {
    bool  caret       = false;      // input: draw Caret markers
    bool  negativeRed = false;      // output: tint negative numbers red
    Color ink         = col::text;  // main ink (dim for disabled equation rows)
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct Node {
    enum Kind {
        HBox,         // horizontal list; kids spaced by the TeX glue table
        Text,         // upright text via Canvas::text (digits, "sin", π fallback)
        Glyph,        // one math-face glyph by key (italic vars, axis operators)
        Frac,         // kids = {num, den}
        Radical,      // kids = {radicand[, degree]}
        Script,       // kids = {base, script}; sup by default, sub when `sub`
        Delim,        // kids = {child}; cycled/stretchy open+close pair around it
        Placeholder,  // dashed empty-slot box (editor slots, orphan power base)
        Caret,        // zero-width cursor marker (drawn only when Style::caret)
    };
    Kind        kind;
    std::string text;                  // Text
    uint32_t    key = 0;               // Glyph
    MathClass   cls = MathClass::Ord;  // spacing class when an HBox child
    bool        negative = false;      // Text/Glyph: negative-number tint
    bool        hasColor = false;      // explicit colour (cycled parens, ghosts)
    Color       color{};
    int         level = 0;             // Delim: inside-out cycling level
    bool        scriptCaret = false;   // Script: caret between base and exponent
    bool        sub = false;           // Script: subscript instead of superscript
    bool        ghostClose = false;    // Delim: close drawn dim (unclosed input paren)
    std::vector<NodePtr> kids;

    // Layout cache — filled by layout(), reused by draw().
    Box       box{};
    float     size = 0;                // resolved px size of this node
    float     gapBefore = 0;           // TeX glue, set by the parent HBox
    MathStyle style = MathStyle::Display;

    explicit Node(Kind k) : kind(k) {}
};

inline NodePtr mk(Node::Kind k) { return std::make_unique<Node>(k); }
inline NodePtr mkText(std::string s, MathClass cl = MathClass::Ord) {
    auto n = mk(Node::Text); n->text = std::move(s); n->cls = cl; return n;
}
inline NodePtr mkGlyph(uint32_t key, MathClass cl = MathClass::Ord) {
    auto n = mk(Node::Glyph); n->key = key; n->cls = cl; return n;
}

// Resolve styles/sizes and cache every node's Box. `displaySize` is the px size
// of Display-style material; Script/ScriptScript nodes scale down by the font's
// scriptPercentScaleDown constants. Returns the root box.
Box layout(Canvas& c, Node& n, float displaySize, MathStyle st = MathStyle::Display);

// Draw a laid-out tree with its left edge at x and its math axis at yAxis.
// layout() must have run on this tree (same canvas/sizes) first.
void draw(Canvas& c, const Node& n, float x, float yAxis, const Style& s);

// ── Shared low-level typography (used by the layout core, the builders in
//    mathlayout.cc, and the keypad icons) ────────────────────────────────────
// Math-axis height as a fraction of em (MATH table axisHeight, fallback 0.25).
float axisFrac(Canvas& c);
// Nominal atom ink extents about the math axis.
float atomAbove(Canvas& c, float size);
float atomBelow(Canvas& c, float size);
// Draw text with its BASELINE axisHeight·size below yAxis (math-axis aligned).
void axisText(Canvas& c, std::string_view s, float x, float yAxis, float size, Color col);
// Size multiplier for a style (1.0 / scriptPercentScaleDown / scriptScript…).
float styleScale(Canvas& c, MathStyle st);
// Math-italic codepoint for a variable letter (U+1D44E block; italic h = U+210E).
uint32_t mathItalicCp(char ch);
// OpenType-MATH radical: full box of a radical wrapping `radicand`, and drawing
// the surd + vinculum (returns the x where the radicand content starts).
Box   radicalMeasure(Canvas& c, const Box& radicand, float size);
float radicalDraw(Canvas& c, const Box& radicand, float x, float yAxis, float size, Color col);
// Dashed placeholder rectangle (editor empty-slot style).
void dashedRect(Canvas& c, float x, float y, float w, float h, Color col);
// Nested-delimiter cycling: level counts INSIDE-OUT (innermost pair = 0 = "(").
// Shape cycles ( ) → [ ] → { } every level; colour cycles every 3 levels.
const char* openDelim(int level);
const char* closeDelim(int level);
Color delimColor(int level);

}  // namespace mathbox
