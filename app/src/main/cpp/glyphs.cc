#include "glyphs.hh"
#include "renderer.hh"
#include <algorithm>

namespace {

// One stroke segment in unit-glyph space (0..1, y-down).
struct Seg { float x0, y0, x1, y1; };

struct GlyphDef {
  const Seg* segs;
  int        count;
  float      advance;   // in unit-glyph space; default 1.1 (slight gap)
};

// ── Glyph stroke tables ────────────────────────────────────────────────
// Each glyph fits in a 1x1 box. Stencil/digital style. y=0 is top.

// 0  outline rect
static const Seg seg_0[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.9f, 0.1f, 0.9f, 0.9f},
  {0.9f, 0.9f, 0.1f, 0.9f},
  {0.1f, 0.9f, 0.1f, 0.1f},
};
// 1  vertical bar + small top serif + small base
static const Seg seg_1[] = {
  {0.5f, 0.05f, 0.5f, 0.95f},
  {0.3f, 0.20f, 0.5f, 0.05f},
  {0.3f, 0.95f, 0.7f, 0.95f},
};
// 2
static const Seg seg_2[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.9f, 0.1f, 0.9f, 0.5f},
  {0.9f, 0.5f, 0.1f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};
// 3
static const Seg seg_3[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.9f, 0.1f, 0.9f, 0.5f},
  {0.4f, 0.5f, 0.9f, 0.5f},
  {0.9f, 0.5f, 0.9f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};
// 4
static const Seg seg_4[] = {
  {0.1f, 0.1f, 0.1f, 0.5f},
  {0.1f, 0.5f, 0.9f, 0.5f},
  {0.7f, 0.1f, 0.7f, 0.95f},
};
// 5
static const Seg seg_5[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.1f, 0.1f, 0.1f, 0.5f},
  {0.1f, 0.5f, 0.9f, 0.5f},
  {0.9f, 0.5f, 0.9f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};
// 6
static const Seg seg_6[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.1f, 0.1f, 0.1f, 0.9f},
  {0.1f, 0.5f, 0.9f, 0.5f},
  {0.9f, 0.5f, 0.9f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};
// 7
static const Seg seg_7[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.9f, 0.1f, 0.3f, 0.9f},
};
// 8  outline + middle bar
static const Seg seg_8[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.9f, 0.1f, 0.9f, 0.9f},
  {0.9f, 0.9f, 0.1f, 0.9f},
  {0.1f, 0.9f, 0.1f, 0.1f},
  {0.1f, 0.5f, 0.9f, 0.5f},
};
// 9
static const Seg seg_9[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.1f, 0.1f, 0.1f, 0.5f},
  {0.1f, 0.5f, 0.9f, 0.5f},
  {0.9f, 0.1f, 0.9f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};
// .  small square at the bottom
static const Seg seg_dot[] = {
  {0.40f, 0.85f, 0.60f, 0.85f},
  {0.60f, 0.85f, 0.60f, 0.95f},
  {0.60f, 0.95f, 0.40f, 0.95f},
  {0.40f, 0.95f, 0.40f, 0.85f},
};
// +
static const Seg seg_plus[] = {
  {0.2f, 0.5f, 0.8f, 0.5f},
  {0.5f, 0.2f, 0.5f, 0.8f},
};
// -
static const Seg seg_minus[] = {
  {0.2f, 0.5f, 0.8f, 0.5f},
};
// x  (multiply)
static const Seg seg_x[] = {
  {0.2f, 0.2f, 0.8f, 0.8f},
  {0.2f, 0.8f, 0.8f, 0.2f},
};
// /  (divide)
static const Seg seg_slash[] = {
  {0.2f, 0.9f, 0.8f, 0.1f},
};
// =
static const Seg seg_eq[] = {
  {0.2f, 0.4f, 0.8f, 0.4f},
  {0.2f, 0.6f, 0.8f, 0.6f},
};
// %  diagonal slash + two square dots
static const Seg seg_percent[] = {
  {0.05f, 0.95f, 0.95f, 0.05f},
  // top-left dot
  {0.05f, 0.05f, 0.30f, 0.05f},
  {0.30f, 0.05f, 0.30f, 0.30f},
  {0.30f, 0.30f, 0.05f, 0.30f},
  {0.05f, 0.30f, 0.05f, 0.05f},
  // bottom-right dot
  {0.70f, 0.70f, 0.95f, 0.70f},
  {0.95f, 0.70f, 0.95f, 0.95f},
  {0.95f, 0.95f, 0.70f, 0.95f},
  {0.70f, 0.95f, 0.70f, 0.70f},
};
// r → radical (sqrt) sign: small stub, tall diagonal up, top bar
static const Seg seg_radical[] = {
  {0.05f, 0.55f, 0.20f, 0.70f},
  {0.20f, 0.70f, 0.40f, 0.20f},
  {0.40f, 0.20f, 0.95f, 0.20f},
};
// A
static const Seg seg_A[] = {
  {0.1f, 0.9f, 0.5f, 0.1f},
  {0.5f, 0.1f, 0.9f, 0.9f},
  {0.25f, 0.6f, 0.75f, 0.6f},
};
// N
static const Seg seg_N[] = {
  {0.1f, 0.1f, 0.1f, 0.9f},
  {0.9f, 0.1f, 0.9f, 0.9f},
  {0.1f, 0.1f, 0.9f, 0.9f},
};
// S  (same shape as 5)
static const Seg seg_S[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.1f, 0.1f, 0.1f, 0.5f},
  {0.1f, 0.5f, 0.9f, 0.5f},
  {0.9f, 0.5f, 0.9f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};
// C
static const Seg seg_C[] = {
  {0.1f, 0.1f, 0.9f, 0.1f},
  {0.1f, 0.1f, 0.1f, 0.9f},
  {0.1f, 0.9f, 0.9f, 0.9f},
};

#define G(a, ad) GlyphDef{ a, (int)(sizeof(a)/sizeof(a[0])), ad }

const GlyphDef* lookup(char c) {
  static const GlyphDef d0 = G(seg_0, 1.1f);
  static const GlyphDef d1 = G(seg_1, 1.1f);
  static const GlyphDef d2 = G(seg_2, 1.1f);
  static const GlyphDef d3 = G(seg_3, 1.1f);
  static const GlyphDef d4 = G(seg_4, 1.1f);
  static const GlyphDef d5 = G(seg_5, 1.1f);
  static const GlyphDef d6 = G(seg_6, 1.1f);
  static const GlyphDef d7 = G(seg_7, 1.1f);
  static const GlyphDef d8 = G(seg_8, 1.1f);
  static const GlyphDef d9 = G(seg_9, 1.1f);
  static const GlyphDef dDot = G(seg_dot, 0.6f);
  static const GlyphDef dPlus = G(seg_plus, 1.1f);
  static const GlyphDef dMinus = G(seg_minus, 1.1f);
  static const GlyphDef dX = G(seg_x, 1.1f);
  static const GlyphDef dSlash = G(seg_slash, 1.1f);
  static const GlyphDef dEq = G(seg_eq, 1.1f);
  static const GlyphDef dPercent = G(seg_percent, 1.1f);
  static const GlyphDef dRad = G(seg_radical, 1.1f);
  static const GlyphDef dA = G(seg_A, 1.1f);
  static const GlyphDef dN = G(seg_N, 1.1f);
  static const GlyphDef dS = G(seg_S, 1.1f);
  static const GlyphDef dC = G(seg_C, 1.1f);

  switch (c) {
    case '0': return &d0;
    case '1': return &d1;
    case '2': return &d2;
    case '3': return &d3;
    case '4': return &d4;
    case '5': return &d5;
    case '6': return &d6;
    case '7': return &d7;
    case '8': return &d8;
    case '9': return &d9;
    case '.': return &dDot;
    case '+': return &dPlus;
    case '-': return &dMinus;
    case 'x': return &dX;
    case '/': return &dSlash;
    case '=': return &dEq;
    case '%': return &dPercent;
    case 'r': return &dRad;
    case 'A': return &dA;
    case 'N': return &dN;
    case 'S': return &dS;
    case 'C': return &dC;
    default:  return nullptr;
  }
}

void pushCurve(std::vector<float>& out,
               float type, float a, float b, float c, float d,
               float r, float g, float bb, float aa, float lineWidth,
               float minX, float minY, float maxX, float maxY) {
  size_t n = out.size();
  out.resize(n + Renderer::CURVE_FLOATS, 0.0f);
  out[n + 0]  = type;
  out[n + 1]  = a;
  out[n + 2]  = b;
  out[n + 3]  = c;
  out[n + 4]  = d;
  out[n + 5]  = r;
  out[n + 6]  = g;
  out[n + 7]  = bb;
  out[n + 8]  = aa;
  out[n + 9]  = lineWidth;
  out[n + 10] = minX;
  out[n + 11] = minY;
  out[n + 12] = maxX;
  out[n + 13] = maxY;
}

} // namespace

void emitFilledRect(std::vector<float>& out,
                    float cx, float cy, float halfW, float halfH,
                    float r, float g, float b, float a) {
  pushCurve(out, /*type=*/2.0f,
            cx, cy, halfW, halfH,
            r, g, b, a,
            /*lineWidth=*/0.0f,
            cx - halfW - 1.0f, cy - halfH - 1.0f,
            cx + halfW + 1.0f, cy + halfH + 1.0f);
}

void emitLineSegment(std::vector<float>& out,
                     float x0, float y0, float x1, float y1,
                     float lineWidth,
                     float r, float g, float b, float a) {
  float pad = lineWidth + 1.5f;
  pushCurve(out, /*type=*/3.0f,
            x0, y0, x1, y1,
            r, g, b, a,
            lineWidth,
            std::min(x0, x1) - pad, std::min(y0, y1) - pad,
            std::max(x0, x1) + pad, std::max(y0, y1) + pad);
}

void emitGlyph(std::vector<float>& out,
               char c, float x, float y, float scale, float lineWidth,
               float r, float g, float b, float a) {
  const GlyphDef* def = lookup(c);
  if (!def) return;
  for (int i = 0; i < def->count; i++) {
    const Seg& s = def->segs[i];
    emitLineSegment(out,
                    x + s.x0 * scale, y + s.y0 * scale,
                    x + s.x1 * scale, y + s.y1 * scale,
                    lineWidth, r, g, b, a);
  }
}

float glyphAdvance(char c, float scale) {
  const GlyphDef* def = lookup(c);
  if (!def) return 0.5f * scale;     // unknown chars take half-cell space
  return def->advance * scale;
}
