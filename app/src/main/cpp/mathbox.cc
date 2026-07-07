// mathbox.cc — TeX-style math layout core (see mathbox.hh).
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "mathbox.hh"

#include <algorithm>

#include "msdf.hh"

namespace mathbox {

// ── Shared low-level typography ──────────────────────────────────────────────

float axisFrac(Canvas& c) {
    const MsdfFont* mf = c.msdfFont();
    return (mf && mf->hasMath()) ? mf->mathConstants().axisHeight() : 0.25f;
}
// Atom ink extents measured from the math axis (not the baseline): cap/ascent
// rises kAscent−axis above it; the baseline sits axis below it (+ descender room).
float atomAbove(Canvas& c, float size) { return (0.70f - axisFrac(c)) * size; }
float atomBelow(Canvas& c, float size) { return (axisFrac(c) + 0.22f) * size; }

void axisText(Canvas& c, std::string_view s, float x, float yAxis, float size, Color col) {
    c.text(s, x, yAxis - (1.0f - axisFrac(c)) * size, size, col);
}

float styleScale(Canvas& c, MathStyle st) {
    if (st == MathStyle::Display || st == MathStyle::Text) return 1.0f;
    const MsdfFont* mf = c.msdfFont();
    float s = 0.0f;
    if (mf && mf->hasMath()) {
        const MathConstants& mc = mf->mathConstants();
        s = st == MathStyle::Script ? mc.scriptPercentScaleDown()
                                    : mc.scriptScriptPercentScaleDown();
    }
    if (s > 0.3f && s < 1.0f) return s;
    return st == MathStyle::Script ? 0.7f : 0.5f;        // sane fallbacks
}

uint32_t mathItalicCp(char ch) {
    if (ch == 'h')              return 0x210E;                       // italic h hole
    if (ch >= 'a' && ch <= 'z') return 0x1D44E + (uint32_t)(ch - 'a');
    if (ch >= 'A' && ch <= 'Z') return 0x1D434 + (uint32_t)(ch - 'A');
    return 0;
}

const char* openDelim(int level) {
    switch (level % 3) { case 1: return "["; case 2: return "{"; default: return "("; }
}
const char* closeDelim(int level) {
    switch (level % 3) { case 1: return "]"; case 2: return "}"; default: return ")"; }
}
Color delimColor(int level) {
    switch ((level / 3) % 3) { case 1: return col::green; case 2: return col::accent; default: return col::text; }
}

void dashedRect(Canvas& c, float x, float y, float w, float h, Color col) {
    float dash = std::max(2.0f, h * 0.14f), step = dash * 2.0f;
    float t = std::max(1.0f, h * 0.05f);
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

// ── OpenType-MATH radical (surd from the font's vertical construction +
//    vinculum from MathConstants; legacy √-scaling fallback without a font) ──
namespace {

// True ink extents of one glyph about the BASELINE, in em (above positive up).
// Plane boxes carry the MSDF margin, so real ink edges are plane ± glyphPadEm.
bool inkExtents(const MsdfFont* mf, uint32_t key, float& aboveEm, float& belowEm) {
    if (!mf) return false;
    const MsdfGlyph* g = mf->glyphByKey(key);
    if (!g || !g->hasGlyph) return false;
    float pad = mf->glyphPadEm();
    aboveEm = -(g->planeT + pad);
    belowEm =  (g->planeB - pad);
    return true;
}

struct MathRadInfo {
    bool     ok = false;
    VStretch vs;
    float    advEm = 0.0f;
    float    gapEm = 0.0f;   // radicand→rule clearance, excess-adjusted (TeX rule 11)
    float    inkEm = 0.0f;   // true ink height of the chosen surd
};

MathRadInfo radSurd(Canvas& c, const Box& r, float size) {
    MathRadInfo o;
    const MsdfFont* mf = c.msdfFont();
    if (!mf || !mf->hasMath()) return o;
    uint32_t radKey = mf->mathKey(0x221A);
    const MathConstruction* rc = radKey ? mf->construction(radKey) : nullptr;
    if (!rc) return o;
    const MathConstants& mc = mf->mathConstants();
    float pad = mf->glyphPadEm();
    // Our surfaces are display style → the roomier display gap when the font has it.
    float gap = mc.radicalDisplayStyleVerticalGap();
    if (!(gap > 0.0f)) gap = mc.radicalVerticalGap();
    float contentEm = (r.above + r.below) / size;
    float targetEm  = contentEm + gap + mc.radicalRuleThickness();
    o.vs = mf->buildVStretch(*rc, targetEm);
    // True ink height of what we'll draw (a pre-built variant is usually taller
    // than the target — variants come in discrete sizes).
    o.inkEm = o.vs.heightEm;
    if (o.vs.single) {
        const MsdfGlyph* g = mf->glyphByKey(o.vs.key);
        if (g) o.inkEm = g->planeB - g->planeT - 2.0f * pad;
    }
    // TeX Appendix G rule 11: when the chosen surd is taller than needed, HALF
    // the excess raises the rule (grows the gap above the radicand) and half
    // hangs below — not all of it below, which left the tail dangling under
    // short radicands.
    o.gapEm = gap + std::max(0.0f, o.inkEm - targetEm) * 0.5f;
    uint32_t barKey = o.vs.single ? o.vs.key : (o.vs.parts.empty() ? radKey : o.vs.parts.back().key);
    const MsdfGlyph* bg = mf->glyphByKey(barKey);
    o.advEm = bg ? bg->advance : 0.6f;
    o.ok = true;
    return o;
}

}  // namespace

Box radicalMeasure(Canvas& c, const Box& r, float size) {
    MathRadInfo o = radSurd(c, r, size);
    if (!o.ok) {  // legacy fallback (no MATH font)
        float gs = r.above + r.below + size * 0.16f;
        return {c.textWidth("\xE2\x88\x9A", gs) * 0.9f + r.w + size * 0.12f,
                r.above + size * 0.16f, r.below};
    }
    const MsdfFont* mf = c.msdfFont();
    const MathConstants& mc = mf->mathConstants();
    float vgap = o.gapEm * size;
    float rule = std::max(1.0f, mc.radicalRuleThickness() * size);
    float extra = mc.radicalExtraAscender() * size;
    float gapXR = size * 0.05f, endPad = size * 0.05f;
    float aboveExtent = r.above + vgap + rule;
    float below = std::max(r.below, o.inkEm * size - aboveExtent);
    return {o.advEm * size + gapXR + r.w + endPad, aboveExtent + extra, below};
}

float radicalDraw(Canvas& c, const Box& r, float x, float yAxis, float size, Color col) {
    MathRadInfo o = radSurd(c, r, size);
    if (!o.ok) {  // legacy fallback (no MATH font)
        float gs = r.above + r.below + size * 0.16f;
        float gw = c.textWidth("\xE2\x88\x9A", gs), t = std::max(1.5f, size * 0.05f);
        c.text("\xE2\x88\x9A", x, (yAxis + r.below) - gs, gs, col);
        float radX = x + gw * 0.9f;
        c.rect(radX, (yAxis - r.above) - size * 0.08f, r.w + size * 0.10f, t, col);
        return radX;
    }
    const MsdfFont* mf = c.msdfFont();
    const MathConstants& mc = mf->mathConstants();
    float pad = mf->glyphPadEm();
    float vgap = o.gapEm * size;
    float rule = std::max(1.0f, mc.radicalRuleThickness() * size);
    float ruleTopY = (yAxis - r.above) - vgap - rule;
    if (o.vs.single) {
        const MsdfGlyph* g = mf->glyphByKey(o.vs.key);
        float planeT = g ? g->planeT : -(o.vs.heightEm + pad);
        c.mathGlyph(o.vs.key, x, ruleTopY - (planeT + pad) * size, size, col);
    } else {
        float bottomY = ruleTopY + o.vs.heightEm * size;
        for (const PlacedPart& pp : o.vs.parts) {
            const MsdfGlyph* g = mf->glyphByKey(pp.key);
            float planeB = g ? g->planeB : 0.0f;
            c.mathGlyph(pp.key, x, bottomY - pp.bottomEm * size - (planeB - pad) * size, size, col);
        }
    }
    float barStartX = x + o.advEm * size;
    float gapXR = size * 0.05f, endPad = size * 0.05f;
    float radX = barStartX + gapXR;
    c.rect(barStartX, ruleTopY, gapXR + r.w + endPad, rule, col);
    return radX;
}

// ── Internal layout helpers ──────────────────────────────────────────────────
namespace {

// TeX style stepping (TeXbook Appendix G): fraction numerator/denominator go
// one style down; a superscript goes to the script of the current style.
MathStyle fracChildStyle(MathStyle st) {
    switch (st) {
        case MathStyle::Display: return MathStyle::Text;
        case MathStyle::Text:    return MathStyle::Script;
        default:                 return MathStyle::ScriptScript;
    }
}
MathStyle scriptChildStyle(MathStyle st) {
    return st <= MathStyle::Text ? MathStyle::Script : MathStyle::ScriptScript;
}
bool scriptish(MathStyle st) { return st >= MathStyle::Script; }

// TeXbook ch.18 inter-class spacing (rows = left class, cols = right class).
// 0 none · 1 thin (3/18 em) · 2 med (4/18) · 3 thick (5/18); +4 ⇒ suppressed in
// Script/ScriptScript styles. Impossible ('*') entries are 0.
constexpr uint8_t kGlue[8][8] = {
    //           Ord   Op   Bin   Rel  Open Close Punct Inner
    /*Ord*/   {   0,    1,  2+4,  3+4,   0,    0,    0,  1+4 },
    /*Op*/    {   1,    1,    0,  3+4,   0,    0,    0,  1+4 },
    /*Bin*/   { 2+4,  2+4,    0,    0, 2+4,    0,    0,  2+4 },
    /*Rel*/   { 3+4,  3+4,    0,    0, 3+4,    0,    0,  3+4 },
    /*Open*/  {   0,    0,    0,    0,   0,    0,    0,    0 },
    /*Close*/ {   0,    1,  2+4,  3+4,   0,    0,    0,  1+4 },
    /*Punct*/ { 1+4,  1+4,    0,  1+4, 1+4,  1+4,  1+4,  1+4 },
    /*Inner*/ { 1+4,    1,  2+4,  3+4, 1+4,    0,  1+4,  1+4 },
};

float glueEm(MathClass l, MathClass r, MathStyle st) {
    uint8_t e = kGlue[(int)l][(int)r];
    if ((e & 4) && scriptish(st)) return 0.0f;
    switch (e & 3) {
        case 1: return 3.0f / 18.0f;
        case 2: return 4.0f / 18.0f;
        case 3: return 5.0f / 18.0f;
    }
    return 0.0f;
}

// TeX's Bin demotion: a binary operator with no proper operand on a side is
// really a sign/ordinary symbol — Bin after nothing/Bin/Op/Rel/Open/Punct
// becomes Ord, and Bin before Rel/Close/Punct/nothing becomes Ord. This is
// what makes a leading '−' hug its operand while a true binary '−' breathes.
void demoteBins(const std::vector<Node*>& v, std::vector<MathClass>& eff) {
    int n = (int)v.size();
    eff.resize(n);
    for (int i = 0; i < n; ++i) {
        eff[i] = v[i]->cls;
        if (eff[i] == MathClass::Bin) {
            bool demote = i == 0;
            if (!demote) {
                MathClass p = eff[i - 1];
                demote = p == MathClass::Bin || p == MathClass::Op || p == MathClass::Rel ||
                         p == MathClass::Open || p == MathClass::Punct;
            }
            if (demote) eff[i] = MathClass::Ord;
        }
    }
    for (int i = n - 1; i >= 0; --i) {
        if (eff[i] != MathClass::Bin) continue;
        bool demote = i == n - 1;
        if (!demote) {
            MathClass x = v[i + 1]->cls;
            demote = x == MathClass::Rel || x == MathClass::Close || x == MathClass::Punct;
        }
        if (demote) eff[i] = MathClass::Ord;
    }
}

// ── Fraction geometry: TeX's rules with the font's MATH constants. The rule is
//    centred on the math axis and exactly as wide as the wider child (plus a
//    whisker of overhang); children sit at the style's shift-up/-down, pushed
//    further only if the minimum gap to the rule demands it. `cs` is the child
//    render size (one style below the fraction's own). ──
struct FracL {
    float rule = 0, pad = 0, barW = 0;
    float numDy = 0, denDy = 0, numX = 0, denX = 0;
    Box   box;
};

FracL fracLayout(Canvas& c, const Box& num, const Box& den,
                 float size, float cs, bool display) {
    FracL L;
    const MsdfFont* mf = c.msdfFont();
    float inner = 0.0f;              // TeX: the rule is EXACTLY the content width
    L.pad = size * 0.12f;            // TeX \nulldelimiterspace (1.2pt at 10pt)
    if (mf && mf->hasMath()) {
        const MathConstants& mc = mf->mathConstants();
        float ax = mc.axisHeight();
        L.rule = std::max(1.0f, mc.fractionRuleThickness() * size);
        float up   = display ? mc.fractionNumeratorDisplayStyleShiftUp()
                             : mc.fractionNumeratorShiftUp();
        float down = display ? mc.fractionDenominatorDisplayStyleShiftDown()
                             : mc.fractionDenominatorShiftDown();
        float gapN = display ? mc.fractionNumDisplayStyleGapMin()
                             : mc.fractionNumeratorGapMin();
        float gapD = display ? mc.fractionDenomDisplayStyleGapMin()
                             : mc.fractionDenominatorGapMin();
        // Shift constants are baseline-relative; our children hang off their own
        // math axis, whose baseline sits ax·cs below it — translate accordingly.
        L.numDy = std::min((ax - up) * size - ax * cs,
                           -(L.rule * 0.5f + gapN * size + num.below));
        L.denDy = std::max((ax + down) * size - ax * cs,
                           L.rule * 0.5f + gapD * size + den.above);
    } else {                         // legacy heuristics (no MATH font)
        L.rule  = std::max(1.5f, size * 0.06f);
        float gap = size * 0.20f;
        L.numDy = -(gap + L.rule * 0.5f + num.below);
        L.denDy =   gap + L.rule * 0.5f + den.above;
    }
    L.barW = std::max(num.w, den.w) + 2.0f * inner;
    L.numX = L.pad + (L.barW - num.w) * 0.5f;
    L.denX = L.pad + (L.barW - den.w) * 0.5f;
    L.box  = {L.barW + 2.0f * L.pad, num.above - L.numDy, L.denDy + den.below};
    return L;
}

// ── Delimiters. A normal-size delimiter is baseline-aligned with its content
//    (LaTeX log(7)). Content taller than a line stretches the delimiter from the
//    font's VERTICAL CONSTRUCTION (pre-built variants / assembly recipe — the
//    same machinery as the radical, crisp at any height), sized by TeX's
//    \delimiterfactor (901/1000 of twice the content's extent about the axis)
//    and CENTRED ON THE MATH AXIS. Scaled text remains only as the no-MATH-font
//    fallback. ──
bool tallDelim(float size, const Box& a) { return (a.above + a.below) > size * 1.30f; }

struct DelimL {
    int      mode = 0;      // 0 plain glyph · 1 construction stretch · 2 legacy scale
    VStretch vs;
    float    w = 0;         // advance (px) of what will be drawn
    float    inkEm = 0;     // ink height (em) in mode 1
};

DelimL delimLayout(Canvas& c, const char* g, const Box& a, float size) {
    DelimL L;
    if (!tallDelim(size, a)) { L.w = c.textWidth(g, size); return L; }
    const MsdfFont* mf = c.msdfFont();
    uint32_t key = (mf && mf->hasMath())
                       ? mf->mathKey(static_cast<uint32_t>(static_cast<unsigned char>(g[0])))
                       : 0;
    const MathConstruction* con = key ? mf->construction(key) : nullptr;
    if (!con) {  // no math font / no construction → legacy scaled text glyph
        L.mode = 2;
        L.w = c.textWidth(g, (a.above + a.below) * 1.05f);
        return L;
    }
    float targetEm = 2.0f * std::max(a.above, a.below) / size * 0.901f;  // \delimiterfactor
    L.vs = mf->buildVStretch(*con, targetEm);
    L.inkEm = L.vs.heightEm;
    uint32_t wk = key;
    if (L.vs.single) {
        wk = L.vs.key;
        const MsdfGlyph* gg = mf->glyphByKey(L.vs.key);
        if (gg) L.inkEm = gg->planeB - gg->planeT - 2.0f * mf->glyphPadEm();
    } else if (!L.vs.parts.empty()) {
        wk = L.vs.parts.back().key;
    }
    L.w = mf->advanceKey(wk, size);
    L.mode = 1;
    return L;
}

void delimDraw(Canvas& c, const DelimL& L, const char* g, float x, float yAxis,
               float size, const Box& a, Color clr) {
    if (L.mode == 0) {                            // plain, baseline-aligned
        c.text(g, x, yAxis - (1.0f - axisFrac(c)) * size, size, clr);
        return;
    }
    if (L.mode == 2) {                            // legacy fallback (no MATH font)
        float h = (a.above + a.below) * 1.05f;
        c.text(g, x, yAxis - 0.72f * h, h, clr);
        return;
    }
    const MsdfFont* mf = c.msdfFont();
    float pad = mf->glyphPadEm();
    float topY = yAxis - L.inkEm * size * 0.5f;   // centred on the math axis
    if (L.vs.single) {
        const MsdfGlyph* gg = mf->glyphByKey(L.vs.key);
        float planeT = gg ? gg->planeT : -(L.inkEm + pad);
        c.mathGlyph(L.vs.key, x, topY - (planeT + pad) * size, size, clr);
    } else {
        float bottomY = topY + L.vs.heightEm * size;
        for (const PlacedPart& pp : L.vs.parts) {
            const MsdfGlyph* gg = mf->glyphByKey(pp.key);
            float planeB = gg ? gg->planeB : 0.0f;
            c.mathGlyph(pp.key, x, bottomY - pp.bottomEm * size - (planeB - pad) * size,
                        size, clr);
        }
    }
}

// ── Superscript placement per the MATH constants (TeX Appendix G rule 18):
//    a simple (one-glyph) base raises the script baseline SuperscriptShiftUp
//    above its own; a tall base (fraction/radical/chained script) hangs it a
//    fixed drop below its top; and the script's ink bottom must clear
//    SuperscriptBottomMin above the base baseline. A math-italic glyph base
//    also nudges the script right by its baked italic correction (𝑥² hugs the
//    x's slope). SuperscriptBaselineDropMax isn't in the baked 42-field subset;
//    0.25em is Latin Modern's value (the one approximation left — format v3
//    candidate). ──
struct ScriptL { float supDy = 0; float supDx = 0; };  // sup child-axis offsets

// TeX \scriptspace: a whisker of air after every scripted atom (from the font's
// SpaceAfterScript when present).
float spaceAfterScriptEm(Canvas& c) {
    const MsdfFont* mf = c.msdfFont();
    if (mf && mf->hasMath() && mf->mathConstants().spaceAfterScript() > 0.0f)
        return mf->mathConstants().spaceAfterScript();
    return 0.05f;
}

ScriptL scriptLayout(Canvas& c, const Node& sn) {
    const Node& base = *sn.kids[0];
    const Node& sup  = *sn.kids[1];
    const float size = sn.size;
    ScriptL L;
    const MsdfFont* mf = c.msdfFont();
    float ax = axisFrac(c);
    if (sn.sub) {  // ── subscript (TeX rule 18b for subs): shift down + top clearance
        float shiftDn = 0.21f, topMax = 0.37f;
        if (mf && mf->hasMath()) {
            const MathConstants& mc = mf->mathConstants();
            if (mc.subscriptShiftDown() > 0.0f) shiftDn = mc.subscriptShiftDown();
            if (mc.subscriptTopMax()    > 0.0f) topMax  = mc.subscriptTopMax();
        }
        float v = shiftDn * size;                        // sub baseline below base's
        float subTop = sup.box.above + ax * sup.size;    // sub ink top above ITS baseline
        v = std::max(v, subTop - topMax * size);         // top ≤ subscriptTopMax
        L.supDy = (ax * size + v) - ax * sup.size;       // no italic corr for subs
        return L;
    }
    float shiftUp = 0.36f, botMin = 0.25f;
    const float drop = 0.25f;                    // ≈ LM SuperscriptBaselineDropMax
    if (mf && mf->hasMath()) {
        const MathConstants& mc = mf->mathConstants();
        if (mc.superscriptShiftUp()   > 0.0f) shiftUp = mc.superscriptShiftUp();
        if (mc.superscriptBottomMin() > 0.0f) botMin  = mc.superscriptBottomMin();
    }
    bool leaf = base.kind == Node::Text || base.kind == Node::Glyph ||
                base.kind == Node::Placeholder;
    float u = leaf ? shiftUp * size                                  // rule 18a
                   : (base.box.above + ax * size) - drop * size;     // rule 18b
    float supDepth = sup.box.below - ax * sup.size;  // sup ink below its baseline
    u = std::max(u, botMin * size + supDepth);       // rule 18c clearance
    // Base baseline sits ax·size BELOW the shared axis; the sup's own axis sits
    // ax·supSize above the sup baseline.
    L.supDy = (ax * size - u) - ax * sup.size;
    if (base.kind == Node::Glyph && mf) {
        const MsdfGlyph* g = mf->glyphByKey(base.key);
        if (g) L.supDx = g->italic * size;
    }
    return L;
}

// ── Radical degree placement, OpenType/LaTeX style: the degree is kerned INTO
//    the surd's slope (RadicalKernBeforeDegree before it, the NEGATIVE
//    RadicalKernAfterDegree after it), and its bottom is raised
//    RadicalDegreeBottomRaisePercent of the radical's total height up from the
//    radical's bottom — so ³√x nestles the 3 into the crook of the √. ──
struct DegreeL {
    float kernB = 0;    // x of the degree from the radical's left edge
    float surdX = 0;    // x where the surd starts (degree tucks over its slope)
    float axisDy = 0;   // degree child-axis y offset from the radical's axis
};

DegreeL degreeLayout(Canvas& c, const Box& rad /*surd box, no degree*/,
                     const Box& deg, float size) {
    const MsdfFont* mf = c.msdfFont();
    float kb = 0.28f, ka = -0.42f, raise = 0.6f;         // fallbacks (no MATH font)
    if (mf && mf->hasMath()) {
        const MathConstants& mc = mf->mathConstants();
        kb = mc.radicalKernBeforeDegree();
        ka = mc.radicalKernAfterDegree();
        float r = mc.radicalDegreeBottomRaisePercent();
        if (r > 0.2f && r < 0.95f) raise = r;
    }
    DegreeL L;
    L.kernB = std::max(0.0f, kb * size);
    L.surdX = std::max(0.0f, L.kernB + deg.w + ka * size);
    float H = rad.above + rad.below;                     // total radical height
    float degBottom = rad.below - raise * H;             // y (down) rel. the axis
    L.axisDy = degBottom - deg.below;
    return L;
}

// Placeholder box dimensions (the editor's dashed empty slot).
Box placeholderBox(float size) { return {size * 0.55f, size * 0.35f, size * 0.35f}; }

}  // namespace

// ── layout() ─────────────────────────────────────────────────────────────────

Box layout(Canvas& c, Node& n, float displaySize, MathStyle st) {
    n.style = st;
    n.size  = displaySize * styleScale(c, st);
    const float sz = n.size;
    switch (n.kind) {
        // Leaf boxes hug TRUE INK (TeX measures every clearance — fraction gaps,
        // radical gap, script bottoms — to ink, not to a nominal line). Atoms the
        // atlas can't resolve fall back to the nominal-line box.
        case Node::Text: {
            float above = atomAbove(c, sz), below = atomBelow(c, sz);
            const MsdfFont* mf = c.msdfFont();
            if (mf && !n.text.empty()) {
                bool ok = true;
                float aEm = -1e9f, bEm = -1e9f;
                for (unsigned char ch : n.text) {
                    if (ch >= 0x80) { ok = false; break; }      // multibyte → nominal
                    uint32_t k = mf->keyForStyle(FontStyle::Roman, ch);
                    float a2, b2;
                    if (!k || !inkExtents(mf, k, a2, b2)) { ok = false; break; }
                    aEm = std::max(aEm, a2);
                    bEm = std::max(bEm, b2);
                }
                if (ok) {
                    float ax = axisFrac(c);
                    above = std::max(0.0f, aEm - ax) * sz;
                    below = std::max(0.0f, bEm + ax) * sz;
                }
            }
            n.box = {c.textWidth(n.text, sz), above, below};
            break;
        }
        case Node::Glyph: {
            const MsdfFont* mf = c.msdfFont();
            float above = atomAbove(c, sz), below = atomBelow(c, sz);
            float aEm, bEm;
            if (inkExtents(mf, n.key, aEm, bEm)) {
                float ax = axisFrac(c);
                above = std::max(0.0f, aEm - ax) * sz;
                below = std::max(0.0f, bEm + ax) * sz;
            }
            n.box = {mf ? mf->advanceKey(n.key, sz) : 0.0f, above, below};
            break;
        }
        case Node::Placeholder:
            n.box = placeholderBox(sz);
            break;
        case Node::Caret:
            n.box = {};                       // zero-width marker
            break;
        case Node::HBox: {
            std::vector<Node*> vis;           // spacing context skips Caret markers
            vis.reserve(n.kids.size());
            for (auto& k : n.kids) {
                k->gapBefore = 0.0f;
                if (k->kind != Node::Caret) vis.push_back(k.get());
            }
            std::vector<MathClass> eff;
            demoteBins(vis, eff);
            Box r;
            int vi = 0;
            for (auto& k : n.kids) {
                if (k->kind == Node::Caret) { layout(c, *k, displaySize, st); continue; }
                Box b = layout(c, *k, displaySize, st);
                if (vi > 0) {
                    k->gapBefore = glueEm(eff[vi - 1], eff[vi], st) * sz;
                    if (vis[vi - 1]->kind == Node::Script)      // TeX \scriptspace
                        k->gapBefore += spaceAfterScriptEm(c) * sz;
                }
                r.w += k->gapBefore + b.w;
                r.above = std::max(r.above, b.above);
                r.below = std::max(r.below, b.below);
                ++vi;
            }
            n.box = r;
            break;
        }
        case Node::Frac: {
            MathStyle cst = fracChildStyle(st);
            Box num = layout(c, *n.kids[0], displaySize, cst);
            Box den = layout(c, *n.kids[1], displaySize, cst);
            n.box = fracLayout(c, num, den, sz, n.kids[0]->size,
                               st == MathStyle::Display).box;
            break;
        }
        case Node::Radical: {
            Box r = layout(c, *n.kids[0], displaySize, st);
            Box b = radicalMeasure(c, r, sz);
            if (n.kids.size() > 1) {          // degree index, kerned into the surd
                Box ib = layout(c, *n.kids[1], displaySize, MathStyle::ScriptScript);
                DegreeL L = degreeLayout(c, b, ib, sz);
                b.above = std::max(b.above, -(L.axisDy - ib.above));
                b.w = std::max(L.surdX + b.w, L.kernB + ib.w);
            }
            n.box = b;
            break;
        }
        case Node::Script: {
            Box base = layout(c, *n.kids[0], displaySize, st);
            Box sup  = layout(c, *n.kids[1], displaySize, scriptChildStyle(st));
            ScriptL L = scriptLayout(c, n);
            n.box = {base.w + L.supDx + sup.w,
                     std::max(base.above, -L.supDy + sup.above),
                     std::max(base.below,  L.supDy + sup.below)};
            break;
        }
        case Node::Delim: {
            Box a = layout(c, *n.kids[0], displaySize, st);
            DelimL lo = delimLayout(c, openDelim(n.level), a, sz);
            DelimL rc = delimLayout(c, closeDelim(n.level), a, sz);
            float ext = lo.mode == 1 ? lo.inkEm * sz * 0.5f : 0.0f;
            n.box = {a.w + lo.w + rc.w,
                     std::max(a.above, ext), std::max(a.below, ext)};
            break;
        }
    }
    return n.box;
}

// ── draw() ───────────────────────────────────────────────────────────────────

namespace {

Color leafColor(const Node& n, const Style& s) {
    if (n.negative && s.negativeRed) return col::red;
    return n.hasColor ? n.color : s.ink;
}

// Cycled delimiter colours only make sense on default-ink surfaces; a tinted
// label (e.g. a dimmed disabled equation row) stays uniformly tinted.
bool defaultInk(const Style& s) {
    return s.ink.r == col::text.r && s.ink.g == col::text.g &&
           s.ink.b == col::text.b && s.ink.a == col::text.a;
}
Color delimInk(const Style& s, int level) {
    return defaultInk(s) ? delimColor(level) : s.ink;
}

void drawCaret(Canvas& c, float x, float top, float bot) {
    c.rect(x, top, std::max(1.5f, (bot - top) * 0.04f), bot - top, col::accent);
}

}  // namespace

void draw(Canvas& c, const Node& n, float x, float yAxis, const Style& s) {
    const float sz = n.size;
    switch (n.kind) {
        case Node::Text:
            axisText(c, n.text, x, yAxis, sz, leafColor(n, s));
            return;
        case Node::Glyph:
            c.mathGlyph(n.key, x, yAxis + axisFrac(c) * sz, sz, leafColor(n, s));
            return;
        case Node::Placeholder:
            dashedRect(c, x, yAxis - n.box.above, n.box.w, n.box.above + n.box.below,
                       n.hasColor ? n.color : col::dim);
            return;
        case Node::Caret:
            return;                            // drawn by the parent HBox
        case Node::HBox: {
            float top = yAxis - n.box.above, bot = yAxis + n.box.below;
            float cx = x;
            for (auto& k : n.kids) {
                if (k->kind == Node::Caret) {
                    if (s.caret) drawCaret(c, cx, top, bot);
                    continue;
                }
                cx += k->gapBefore;
                draw(c, *k, cx, yAxis, s);
                cx += k->box.w;
            }
            return;
        }
        case Node::Frac: {
            FracL L = fracLayout(c, n.kids[0]->box, n.kids[1]->box, sz,
                                 n.kids[0]->size, n.style == MathStyle::Display);
            c.rect(x + L.pad, yAxis - L.rule * 0.5f, L.barW, L.rule, s.ink);
            draw(c, *n.kids[0], x + L.numX, yAxis + L.numDy, s);
            draw(c, *n.kids[1], x + L.denX, yAxis + L.denDy, s);
            return;
        }
        case Node::Radical: {
            const Box& r = n.kids[0]->box;
            float surdX = 0.0f;
            if (n.kids.size() > 1) {          // degree first: it sets the surd's x
                Box rb = radicalMeasure(c, r, sz);   // pure surd box (no degree)
                DegreeL L = degreeLayout(c, rb, n.kids[1]->box, sz);
                surdX = L.surdX;
                draw(c, *n.kids[1], x + L.kernB, yAxis + L.axisDy, s);
            }
            float radX = radicalDraw(c, r, x + surdX, yAxis, sz, s.ink);
            draw(c, *n.kids[0], radX, yAxis, s);
            return;
        }
        case Node::Script: {
            const Box& base = n.kids[0]->box;
            ScriptL L = scriptLayout(c, n);
            draw(c, *n.kids[0], x, yAxis, s);
            if (n.scriptCaret && s.caret)
                drawCaret(c, x + base.w, yAxis - base.above, yAxis + base.below);
            draw(c, *n.kids[1], x + base.w + L.supDx, yAxis + L.supDy, s);
            return;
        }
        case Node::Delim: {
            const Box& a = n.kids[0]->box;
            Color dc = delimInk(s, n.level);
            DelimL lo = delimLayout(c, openDelim(n.level), a, sz);
            delimDraw(c, lo, openDelim(n.level), x, yAxis, sz, a, dc);
            draw(c, *n.kids[0], x + lo.w, yAxis, s);
            DelimL rc = delimLayout(c, closeDelim(n.level), a, sz);
            delimDraw(c, rc, closeDelim(n.level), x + lo.w + a.w, yAxis, sz, a,
                      n.ghostClose ? col::dim : dc);   // ghost = still-typing closer
            return;
        }
    }
}

}  // namespace mathbox
