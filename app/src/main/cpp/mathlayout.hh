// mathlayout.hh — 2D ("natural display") rendering entry points.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Thin builders translate the calcedit::Editor tree (live input) and the parsed
// CAS-result AST (mathx::Node, tape) into the shared TeX-style box IR in
// mathbox.hh, which owns ALL the typography — so input and results render with
// one geometry by construction. App-side (depends on Canvas); the editor/math
// models themselves stay Vulkan-free.
#pragma once

class Canvas;
struct Rect;
struct Color;
namespace calcedit { class Editor; }
namespace mathx { struct Node; }

namespace mathlayout {

// Right-align + shrink-to-fit the editor tree inside `panel` and draw it,
// including the cursor caret. Used while the user is typing (not on a result).
void drawEditor(Canvas& c, const calcedit::Editor& ed, const Rect& panel);

// Fitted bounding box of an AST: the size chosen so width ≤ maxWidth (≤ maxSize),
// and the resulting extents above/below the baseline axis. Lets a caller reserve
// the correct height *before* drawing (e.g. variable-height tape rows). Pure
// measurement — no draw, no allocation.
struct AstFit { float size, width, above, below; };
AstFit fitAst(Canvas& c, const mathx::Node& root, float maxSize, float maxWidth);

// Draw an AST at an explicit `size`, right edge at `rightX`, baseline axis at
// `yAxis` (pair with fitAst's size/above to stack rows without overlap).
// `ink` null → result styling (negatives red); non-null → uniform tint (the
// tape's gray source echo).
void drawAstAt(Canvas& c, const mathx::Node& root, float rightX, float yAxis, float size,
               const Color* ink = nullptr);

// GRAPH/TABLE equation labels: "y₁ = <expr>", "r₁ = <expr>" or, when `exprY` is
// given, "(x,y) = (<exprX>, <exprY>)" — the same 2D typography as the RUN tape
// (math-italic lhs letters + subscript index, real fractions/radicals, TeX '='
// spacing). Left-aligned in `panel`, vertically centred. `subIndex` > 0 adds the
// automatic name subscript (𝑦₁, 𝑦₂…); null ASTs render as dashed placeholders;
// `ink` tints the whole label (col::dim for a disabled row). `size` = 0 auto-fits
// to `panel`; pass an explicit size (from fitEqLabelSize) to render several
// labels at ONE uniform size.
void drawEqLabel(Canvas& c, const char* lhs, int subIndex, const mathx::Node* exprX,
                 const mathx::Node* exprY, const Rect& panel, const Color& ink,
                 float size = 0.0f);

// Measure-only: the size drawEqLabel would auto-fit this label to inside `panel`.
// Take the min across rows and pass it back as `size` for a uniform list.
float fitEqLabelSize(Canvas& c, const char* lhs, int subIndex, const mathx::Node* exprX,
                     const mathx::Node* exprY, const Rect& panel);

// Centred math-italic variable name with an optional subscript index (𝑦₁, 𝑥) —
// the TABLE column headers, matching the equation-list names.
void drawVarName(Canvas& c, char var, int subIndex, const Rect& r, const Color& ink);

// Left-aligned "𝑦₁ =" ghost tag for the equation editor's display box: italic
// lhs letters, subscript name index, TeX '=' spacing — matching the list labels.
void drawGhostLhs(Canvas& c, const char* lhs, int subIndex, const Rect& r,
                  const Color& ink);

// ── LaTeX-style key icons (keypad / CAS bar) ─────────────────────────────────
// The keypad's template keys draw the SAME typography the editor/tape render
// with — real math-italic variables, the OpenType-MATH surd, fraction bars and
// dotted placeholder squares — instead of ASCII approximations ("a/b", "x^n").

// Which icon to draw centred in a key.
enum class KeyIcon {
    Var,        // math-italic variable (glyph chosen by `ch`: 'x', 't', …)
    Frac,       // placeholder-over-placeholder fraction:  □/□  (stacked)
    Sqrt,       // radical with a placeholder radicand:    √□
    NthRoot,    // radical with degree + radicand:         ⁿ√□
    Power,      // base placeholder + raised placeholder:  □^□
    Integral,   // ∫ from the math face
    Derivative, // d/dx with math-italic d and x
};

// Draw `icon` centred inside `r` in colour `fg`. `ch` selects the variable glyph
// for KeyIcon::Var (and the variable in Derivative); ignored otherwise. Callers
// draw the button background first (Canvas::rect) — this draws only the "label".
void drawKeyIcon(Canvas& c, KeyIcon icon, const Rect& r, const Color& fg, char ch = 'x');

}  // namespace mathlayout
