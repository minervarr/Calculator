// main.cc — native Vulkan graphing calculator (M4: shell + slide-up keypad).
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Graphing-first (fx-CG500 style): a dominant graph viewport, a status strip, a
// compact icon toolbar, an equation-list overlay (colors / enable / add / delete)
// and a slide-up natural-display keypad that edits the selected equation. Single-
// finger drag pans (or traces); two-finger pinch zooms about the pinch. Run /
// Table modes and SQLite persistence land in M5/M6.
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "canvas_host.hh"
#include "canvas.hh"
#include "fullscreen.hh"
#include "font.hh"
#include "gesture.hh"
#include "mathbox.hh"
#include "mathlayout.hh"
#include "plotview.hh"
#include "saf_bridge.hh"
#include "calc/calc.hh"
#include "calc/editor.hh"
#include "cas/cas_worker.hh"
#include "db/store.hh"
#include "graphing/equation.hh"
#include "graphing/plot.hh"
#include "math/ast.hh"
#include "math/lexer.hh"
#include "math/parser.hh"

#define LOG_TAG "calculator"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

namespace {

struct Key     { Rect r; const char* label; const char* token; };
struct KeySpec { const char* label; const char* token; };

// App modes are an extension "space": Graph + Table are live; Run is a defined
// stub. New modes (more graph types, stats, …) drop into the same registry.
enum class AppMode { Graph, Table, Run };

// One Run/Calc history row. The CAS result is parsed ONCE here on insert; `ast`
// (when non-null) drives the 2D natural-display render, keeping the render loop
// allocation-free. `resultRaw` is the 1D fallback (and future copy/substitution).
struct TapeEntry {
    std::string    label;       // gray source line, e.g. "d/dx √x" (1D fallback)
    std::string    resultRaw;   // raw result string
    mathx::NodePtr ast;         // parsed result for 2D; null → render resultRaw in 1D
    std::string    labelPrefix; // flat lead-in before the 2D echo ("d/dx ", "dec "…)
    mathx::NodePtr labelAst;    // parsed INPUT for the 2D echo; null → flat `label`
};

struct AppState {
    android_app*      app      = nullptr;
    CanvasHost        host;
    Font              font;
    GestureRecognizer gesture;
    plot::PlotView    plotview;
    calcedit::Editor  editor;          // active edit buffer (bound to selectedEq)
    bool              fontLoaded = false;
    bool              dirty      = false;
    bool              viewInit   = false;
    float             navBarPx   = 0.0f;
    double            lastT      = 0.0;
    vce::platform::ImmersiveMode immersiveMode = vce::platform::ImmersiveMode::kFullImmersive;

    // Equations + per-frame sampling scratch (reused → no steady-state alloc).
    std::vector<graphing::Equation>            eqs;
    int                                        nextEqId = 1;
    std::vector<graphing::SamplePt>            sampleBuf;
    std::vector<std::vector<plot::CurvePoint>> curveBufs;
    std::vector<plot::Curve>                   curves;

    bool   degrees = false;

    // Mode + Table view state (table domain is x = tableStart + i*tableStep).
    AppMode mode = AppMode::Graph;
    float   tableScroll = 0.0f;
    bool    tableDrag   = false;
    bool    tableInit   = false;
    double  tableStart  = 0.0, tableEnd = 10.0, tableStep = 1.0;
    std::vector<graphing::SamplePt> tableColBuf[4];  // reused per-column samples
    bool    tableSetupOpen = false;      // Start/End/Step setup modal
    std::string setupBuf[3];             // [0]=start, [1]=end, [2]=step
    int     setupField = 0;

    // Run / Calc mode: a docked keypad feeding the Calc facade + a result tape.
    Calc    runCalc;
    std::vector<TapeEntry> runHistory;  // Run/Calc history (label + result + parsed AST)
    float   runScroll  = 0.0f;          // vertical tape scroll
    float   runScrollX = 0.0f;          // horizontal pan for entries too wide even at the legibility floor
    bool    runDrag    = false;
    double  runEditTime = 0.0;            // last keystroke (live-preview debounce)
    bool    runPreviewDirty = false;
    std::string runPreviewCache;

    // CAS (symbolic) layer: an engine sandboxed on its own thread (the dyno's
    // SymbolicEngine today; Eigenmath/Giac drop in behind the same seam). The
    // render thread only submit()s and poll()s — it never runs the engine.
    cas::CasWorker cas;
    uint64_t       casToken = 0;          // token of the in-flight CAS request
    std::string    casPendingLabel;       // tape label awaiting that reply
    std::string    casPendingPrefix;      // 2D-echo lead-in ("d/dx ", "dec "…)
    mathx::NodePtr casPendingAst;         // parsed input for the 2D echo
    bool           casPending = false;    // a CAS reply is outstanding (keep the loop awake)

    // DEL key press-and-hold repeat, and stylus detection (no haptic for a pen).
    bool    delActive = false;
    double  delDownTime = 0.0, delLastRepeat = 0.0;
    bool    lastToolStylus = false;

    // Persistence: SQLite workspace store + a one-shot restored view window.
    db::Store store;
    bool   persistent  = false;         // store opened OK (else run without saving)
    bool   haveRestore = false;
    double rXmin = 0, rXmax = 0, rYmin = 0, rYmax = 0;
    std::vector<db::SheetInfo> sheets;   // tab bar
    int64_t activeSheet = 0;
    float   tabScroll   = 0.0f;          // horizontal scroll of the tab strip
    bool    tabDrag     = false;
    float   eqScroll    = 0.0f;          // vertical scroll of the equation rows
    bool    eqDrag      = false;
    bool    longPressConsumed = false;   // a long-press handled this gesture

    // Tab long-press menu (Rename / Delete) + the rename keyboard modal.
    bool    sheetMenuOpen = false;
    int64_t menuSheetId   = 0;
    float   menuX = 0, menuY = 0;        // popup anchor (screen px)
    bool    renameOpen    = false;
    int64_t renameSheetId = 0;
    std::string renameBuf;
    bool    renameShift   = false;

    // UI state.
    Rect   plotRect   = {0, 0, 1, 1};
    int    selectedEq = 0;
    int    editField  = 0;              // which field the keypad edits (Parametric: 0=x,1=y)
    bool   listOpen   = false;          // equation-list overlay
    bool   keypadOpen = false;          // slide-up keypad
    float  keypadAnim = 0.0f;           // 0 hidden .. 1 shown

    // Plot interaction.
    bool   panAllowed = false;
    bool   traceMode  = false;
    std::vector<int> traceIds;          // equation ids with a trace marker (multi)
    plot::TraceMarker trace;            // shared cursor state (on + wx)
    std::vector<plot::TraceMarker> traces;  // per-selected-curve markers (per frame)
    float  touchX = 0, touchY = 0;
    bool   pinching = false;
    float  pinchPrevDist = 0.0f;
    float  pinchPrevX = 0.0f, pinchPrevY = 0.0f;  // centroid → two-finger pan
};

db::WorkspaceData snapshot(const AppState* st);  // defined below; used by sheet ops
void closeEqEditor(AppState* st);                // defined below; used by keypadInput

// Default colors for new equations — muted, high-contrast "academic" set.
const graphing::RGBA kPalette[] = {
    {0.290f, 0.565f, 0.886f, 1.0f},  // #4A90E2 blue
    {0.906f, 0.298f, 0.235f, 1.0f},  // #E74C3C crimson
    {0.180f, 0.800f, 0.443f, 1.0f},  // #2ECC71 mint
    {0.945f, 0.769f, 0.059f, 1.0f},  // #F1C40F goldenrod
    {0.608f, 0.349f, 0.714f, 1.0f},  // #9B59B6 amethyst
    {0.102f, 0.737f, 0.612f, 1.0f},  // #1ABC9C turquoise (cycle filler)
};
constexpr int kPaletteN = 6;

double nowSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// Text-cursor blink: visible for the first half of each 1s period (500ms on/off).
bool caretBlinkOn() { return std::fmod(nowSeconds(), 1.0) < 0.5; }

bool loadFontAsset(AAssetManager* mgr, Font& font, const char* path) {
    AAsset* a = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
    if (!a) { LOGE("font asset missing: %s", path); return false; }
    size_t n = AAsset_getLength(a);
    std::vector<uint8_t> buf(n);
    AAsset_read(a, buf.data(), n);
    AAsset_close(a);
    return font.loadFromMemory(buf.data(), n);
}

Color toColor(const graphing::RGBA& c) { return Color{c.r, c.g, c.b, c.a}; }

// Trim `s` with a trailing "..." so it fits within `maxW` px at `size`. UTF-8
// safe: only ever cuts on a codepoint boundary (expressions hold × ÷ − √ π).
std::string ellipsize(Canvas& c, const std::string& s, float size, float maxW) {
    if (maxW <= 0.0f || c.textWidth(s, size) <= maxW) return s;
    float ew = c.textWidth("...", size);
    std::string cur = s;
    while (!cur.empty()) {
        size_t n = cur.size();
        do { --n; } while (n > 0 && (static_cast<unsigned char>(cur[n]) & 0xC0) == 0x80);
        cur.resize(n);
        if (c.textWidth(cur, size) + ew <= maxW) break;
    }
    return cur + "...";
}

float contentHeight(AppState* st) {
    return static_cast<float>(st->host.height()) - st->navBarPx;
}

// A showcase of all three sampling paths (the parameter for polar/parametric is
// the ASCII `t`; θ is presented in the UI label). Defaults give t ∈ [0, 2π].
void seedEquations(AppState* st) {
    graphing::Equation a;                                   // y = f(x)
    a.id = 1; a.type = graphing::EqType::Function; a.expr = "x^2-3"; a.color = kPalette[0];
    graphing::Equation b;                                   // polar 4-petal rose
    b.id = 2; b.type = graphing::EqType::Polar; b.expr = "3*cos(2*t)"; b.color = kPalette[1];
    graphing::Equation c;                                   // parametric Lissajous
    c.id = 3; c.type = graphing::EqType::Parametric;
    c.exprX = "5*cos(t)"; c.exprY = "3*sin(2*t)"; c.color = kPalette[2];
    st->eqs = {a, b, c};
    st->nextEqId = 4;
}

// Renderable = enabled and has the expression(s) its type needs.
bool eqRenderable(const graphing::Equation& e) {
    if (!e.enabled) return false;
    if (e.type == graphing::EqType::Parametric) return !e.exprX.empty() && !e.exprY.empty();
    return !e.expr.empty();  // Function & Polar use the single expr
}

// One-line display, type-aware (θ shown for the polar/parametric parameter).
std::string eqLabel(const graphing::Equation& e) {
    switch (e.type) {
        case graphing::EqType::Polar:
            return "r = " + (e.expr.empty() ? std::string("?") : e.expr);
        case graphing::EqType::Parametric:
            return "(x,y) = (" + (e.exprX.empty() ? std::string("?") : e.exprX) + ", " +
                   (e.exprY.empty() ? std::string("?") : e.exprY) + ")";
        case graphing::EqType::Function:
        default:
            return "y = " + (e.expr.empty() ? std::string("?") : e.expr);
    }
}

// Parse a stored canonical expression into an AST for 2D display (same pattern
// as the RUN tape's result re-parse). Null on empty/unparseable input.
mathx::NodePtr parseExprAst(const std::string& src) {
    if (src.empty()) return nullptr;
    std::vector<mathx::Token> toks;
    if (!mathx::tokenize(src, toks, /*implicitMul=*/true)) return nullptr;
    mathx::ParseResult pr = mathx::parse(toks);
    return (pr.ok && pr.root) ? std::move(pr.root) : nullptr;
}

// Equation label in 2D natural display ("𝑦₁ = <expr>" with real math typography),
// left-aligned in `r`. `subIndex` is the automatic per-type name index (𝑦₁, 𝑟₁…);
// `size` = 0 auto-fits, else renders at the given uniform size. Half-typed strings
// that don't parse fall back to the flat eqLabel() text so nothing disappears.
void drawEqLabel2D(Canvas& c, const graphing::Equation& e, int subIndex,
                   const Rect& r, Color ink, float size = 0.0f) {
    if (e.type == graphing::EqType::Parametric) {
        mathx::NodePtr ax = parseExprAst(e.exprX), ay = parseExprAst(e.exprY);
        bool badX = !ax && !e.exprX.empty(), badY = !ay && !e.exprY.empty();
        if (!badX && !badY) {
            mathlayout::drawEqLabel(c, "(x,y)", 0, ax.get(), ay.get(), r, ink, size);
            return;
        }
    } else {
        mathx::NodePtr a = parseExprAst(e.expr);
        if (a || e.expr.empty()) {
            const char* lhs = e.type == graphing::EqType::Polar ? "r" : "y";
            // exprY = null → single-expression form; null exprX → placeholder □.
            mathlayout::drawEqLabel(c, lhs, subIndex, a.get(), nullptr, r, ink, size);
            return;
        }
    }
    float sz = r.h * 0.38f;                       // flat fallback (unparseable text)
    c.text(ellipsize(c, eqLabel(e), sz, r.w), r.x, r.y + r.h * 0.32f, sz, ink);
}

// Fitted 2D-label size for an equation row; 0 when the row will use the flat
// text fallback (excluded from the shared-size min).
float fitEqLabel2D(Canvas& c, const graphing::Equation& e, int subIndex, const Rect& r) {
    if (e.type == graphing::EqType::Parametric) {
        mathx::NodePtr ax = parseExprAst(e.exprX), ay = parseExprAst(e.exprY);
        if ((!ax && !e.exprX.empty()) || (!ay && !e.exprY.empty())) return 0.0f;
        return mathlayout::fitEqLabelSize(c, "(x,y)", 0, ax.get(), ay.get(), r);
    }
    mathx::NodePtr a = parseExprAst(e.expr);
    if (!a && !e.expr.empty()) return 0.0f;
    const char* lhs = e.type == graphing::EqType::Polar ? "r" : "y";
    return mathlayout::fitEqLabelSize(c, lhs, subIndex, a.get(), nullptr, r);
}

// Short type tag for the equation-list selector button.
const char* eqTypeTag(graphing::EqType t) {
    switch (t) {
        case graphing::EqType::Polar:      return "r(\xCE\xB8)";  // r(θ)
        case graphing::EqType::Parametric: return "x,y";
        case graphing::EqType::Function:
        default:                           return "f(x)";
    }
}

// 1-based position among equations of the SAME type — the automatic stable name
// index (𝑦₁, 𝑦₂, 𝑟₁ …) shared by the eq list, the graph title and the table
// headers. Enable-independent so names never shuffle when toggling curves.
int eqTypeIndex(const AppState* st, const graphing::Equation& e) {
    int n = 0;
    for (const auto& q : st->eqs) {
        if (q.type == e.type) ++n;
        if (&q == &e) return n;
    }
    return n;
}

// ── Bottom icon toolbar ───────────────────────────────────────────────────────
float toolbarHeight(float contentH) { return contentH * 0.085f; }

int toolbarButtons(float W, float contentH, AppMode mode, bool traceMode, Key out[5]) {
    const float botH = toolbarHeight(contentH);
    const float y    = contentH - botH;
    const float pad  = W * 0.018f;
    if (mode != AppMode::Graph) {  // non-graph: just Eqns (graph zoom/trace n/a)
        out[0].r     = {pad, y + botH * 0.12f, W - 2 * pad, botH * 0.76f};
        out[0].label = "Eqns";
        out[0].token = "eqns";
        return 1;
    }
    // While tracing, the zoom pair becomes the FINE-STEP pair: finger drags are
    // the coarse moves, < > nudge the cursor precisely along the locked curve.
    static const char* labels[5]  = {"Eqns", "Trace", "Reset", "\xE2\x88\x92", "+"};
    static const char* toks[5]    = {"eqns", "trace", "reset", "zoomout", "zoomin"};
    static const char* labelsT[5] = {"Eqns", "Trace", "Reset", "<", ">"};
    static const char* toksT[5]   = {"eqns", "trace", "reset", "stepl", "stepr"};
    const char* const* lbl = traceMode ? labelsT : labels;
    const char* const* tok = traceMode ? toksT   : toks;
    const float bw = (W - pad * 6.0f) / 5.0f;
    for (int i = 0; i < 5; i++) {
        out[i].r     = {pad + i * (bw + pad), y + botH * 0.12f, bw, botH * 0.76f};
        out[i].label = lbl[i];
        out[i].token = tok[i];
    }
    return 5;
}

// ── Slide-up keypad ───────────────────────────────────────────────────────────
const KeySpec KP[36] = {
    {"x","x"},     {"(","("},     {")",")"},     {"^","^"},        {"DEL","back"},  {"OK","done"},
    {"7","7"},     {"8","8"},     {"9","9"},     {"\xC3\xB7","/"},  {"a/b","frac"},  {"\xE2\x88\x9A","sqrt"},
    {"4","4"},     {"5","5"},     {"6","6"},     {"\xE2\x8B\x85","*"},  {"x^n","pow"},   {"\xCF\x80","pi"},
    {"1","1"},     {"2","2"},     {"3","3"},     {"\xE2\x88\x92","-"}, {"sin","sin"}, {"cos","cos"},
    {"0","0"},     {".","."},     {"+","+"},     {"tan","tan"},     {"ln","ln"},     {"log","log"},
    {"<","left"},  {">","right"}, {"Up","up"},   {"Dn","down"},     {"e","e"},       {"n\xE2\x88\x9A","nthroot"},
};

float keypadPanelH(float contentH) { return contentH * 0.46f; }

Rect keypadPanelRect(float W, float contentH, float anim) {
    float h = keypadPanelH(contentH);
    return {0.0f, contentH - h * anim, W, h};
}

Rect keypadDispRect(const Rect& panel) {
    float pad = panel.w * 0.025f;
    return {panel.x + pad, panel.y + pad, panel.w - 2 * pad, panel.h * 0.18f};
}

int keypadKeys(const Rect& panel, Key out[36]) {
    float pad   = panel.w * 0.025f;
    float gridY = panel.y + pad + panel.h * 0.18f + pad;
    float gridH = (panel.y + panel.h) - gridY - pad;
    float gridW = panel.w - 2 * pad;
    const int cols = 6, rows = 6;
    float cw = gridW / cols, ch = gridH / rows, bp = pad * 0.30f;
    for (int i = 0; i < 36; i++) {
        int r = i / cols, cc = i % cols;
        out[i].r     = {panel.x + pad + cc * cw + bp, gridY + r * ch + bp, cw - 2 * bp, ch - 2 * bp};
        out[i].label = KP[i].label;
        out[i].token = KP[i].token;
    }
    return 36;
}

void keypadColors(const char* token, Color& bg, Color& fg) {
    // App-local "academic" key palette (kept out of the engine-wide col:: theme).
    const Color kNum  = {0.200f, 0.200f, 0.200f, 1.0f};  // #333333 digits / "."
    const Color kFunc = {0.267f, 0.267f, 0.267f, 1.0f};  // #444444 functions / templates
    const Color kOp   = {0.290f, 0.565f, 0.886f, 1.0f};  // #4A90E2 operators / OK (= plot-1 blue)
    const Color kTxt  = {0.933f, 0.933f, 0.933f, 1.0f};  // #EEEEEE on-key text
    const Color kOnOp = {1.000f, 1.000f, 1.000f, 1.0f};  // pure white on the blue accent

    std::string s = token;
    if (s == "done") { bg = kOp; fg = kOnOp; return; }                    // OK ≈ commit / "="
    if (s == "back") { bg = Color{0.28f, 0.16f, 0.16f, 1.0f}; fg = Color{0.95f, 0.80f, 0.80f, 1.0f}; return; }
    if (s == "+" || s == "-" || s == "*" || s == "/" || s == "^") { bg = kOp; fg = kOnOp; return; }
    if (s == "left" || s == "right" || s == "up" || s == "down")  { bg = kFunc; fg = kOp;  return; }  // nav: blue glyph
    if (s.size() == 1 && ((s[0] >= '0' && s[0] <= '9') || s[0] == '.')) { bg = kNum; fg = kTxt; return; }
    if (s == "x") { bg = kFunc; fg = kOp; return; }   // the plot variable — blue glyph to mark it
    bg = kFunc; fg = kTxt;   // functions / constants / templates
}

std::string eqTokenText(const std::string& t) {
    if (t == "sin") return "sin(";
    if (t == "cos") return "cos(";
    if (t == "tan") return "tan(";
    if (t == "ln")  return "ln(";
    if (t == "log") return "log(";
    return t;
}

graphing::EqType activeType(const AppState* st) {
    if (st->selectedEq < 0 || st->selectedEq >= static_cast<int>(st->eqs.size()))
        return graphing::EqType::Function;
    return st->eqs[st->selectedEq].type;
}

int eqFieldCount(graphing::EqType t) { return t == graphing::EqType::Parametric ? 2 : 1; }

// The editor edits ONE field at a time; resolve which string that is.
std::string& eqFieldRef(graphing::Equation& e, int field) {
    if (e.type == graphing::EqType::Parametric) return field == 1 ? e.exprY : e.exprX;
    return e.expr;  // Function: f(x); Polar: r(t)
}

// Variable key is context-sensitive: x for functions, t for polar/parametric (the
// lexer's only ASCII parameter; θ is decorative in the type tags).
const char* activeVarLabel(const AppState* st) {
    return activeType(st) == graphing::EqType::Function ? "x" : "t";
}

void loadFieldIntoEditor(AppState* st) {
    if (st->selectedEq < 0 || st->selectedEq >= static_cast<int>(st->eqs.size())) return;
    st->editor.loadString(eqFieldRef(st->eqs[st->selectedEq], st->editField));
}

void syncSelectedExpr(AppState* st) {
    if (st->selectedEq < 0 || st->selectedEq >= static_cast<int>(st->eqs.size())) return;
    eqFieldRef(st->eqs[st->selectedEq], st->editField) = st->editor.linearize();
}

void openKeypadFor(AppState* st, int idx) {
    if (idx < 0 || idx >= static_cast<int>(st->eqs.size())) return;
    st->selectedEq = idx;
    st->editField  = 0;
    loadFieldIntoEditor(st);
    st->keypadOpen = true;
    st->listOpen   = false;
    st->dirty = true;
}

// Backspace whichever editor the keypad is bound to (Run's Calc, or an equation).
void backspaceActive(AppState* st) {
    if (st->mode == AppMode::Run) {
        st->runCalc.input("back");
        st->runEditTime = nowSeconds();
        st->runPreviewDirty = true;
    } else {
        st->editor.backspace();
        syncSelectedExpr(st);
    }
    st->dirty = true;
}

bool keypadVisible(const AppState* st) {
    return st->mode == AppMode::Run || st->keypadOpen || st->keypadAnim > 0.001f;
}

// Screen rect of the DEL ("back") key for press-and-hold repeat detection.
Rect delKeyRect(AppState* st) {
    float W = static_cast<float>(st->host.width());
    float anim = (st->mode == AppMode::Run) ? 1.0f : st->keypadAnim;
    Rect panel = keypadPanelRect(W, contentHeight(st), anim);
    Key keys[36];
    keypadKeys(panel, keys);
    for (int i = 0; i < 36; i++)
        if (std::string(keys[i].token) == "back") return keys[i].r;
    return {0, 0, 0, 0};
}

// Run mode: evaluate the current expression and push {expr, result} to the tape.
void runCommit(AppState* st) {
    std::string expr = st->runCalc.displayText();  // prettified current expression
    if (expr.empty()) return;
    std::string raw = st->runCalc.editor().linearize();  // canonical ASCII, pre-'='
    st->runCalc.input("=");
    if (st->runCalc.showingResult()) {
        TapeEntry te;
        te.label     = expr;                       // flat fallback if raw won't parse
        te.resultRaw = st->runCalc.displayText();  // numeric → 1D
        te.labelAst  = parseExprAst(raw);          // 2D echo (real fractions/radicals)
        st->runHistory.push_back(std::move(te));
        if (st->runHistory.size() > 200) st->runHistory.erase(st->runHistory.begin());
        st->runScroll = 0.0f;
        st->runScrollX = 0.0f;
    }
    st->runPreviewCache.clear();
    st->runPreviewDirty = false;
}

// Eigenmath writes surds as fractional exponents (2^(1/2), x^(1/2), (x+1)^(1/2)).
// Rewrite BASE^(1/2) → √BASE so the tape reads like a calculator display. Other
// exponents (^(1/3), ^2, …) are left as-is — the atlas has no ∛ glyph.
std::string casSurds(const std::string& s) {
    const char* kSqrt = "\xE2\x88\x9A";  // √
    auto isWord = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || c == '.';
    };
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 6, "^(1/2)") == 0 && !o.empty()) {
            size_t base = o.size();
            if (o.back() == ')') {              // (...)^(1/2): wrap the whole group
                int depth = 0; size_t j = o.size();
                while (j > 0) {
                    char c = o[--j];
                    if (c == ')') depth++;
                    else if (c == '(' && --depth == 0) break;
                }
                base = j;
            } else {                            // token^(1/2): take the trailing word
                size_t j = o.size();
                while (j > 0 && isWord(o[j - 1])) j--;
                base = j;
            }
            if (base < o.size()) { o.insert(base, kSqrt); i += 6; continue; }  // → √BASE
        }
        o += s[i++];
    }
    return o;
}

// canonical CAS ASCII (e.g. "2*x", "cos(x)", "2^(1/2)") → pretty UTF-8 for the tape.
std::string casPretty(const std::string& in) {
    std::string s = casSurds(in);
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, 5, "sqrt(") == 0) { o += "\xE2\x88\x9A("; i += 5; continue; }  // √(
        if (s.compare(i, 2, "pi")    == 0) { o += "\xCF\x80";      i += 2; continue; }  // π
        char ch = s[i++];
        switch (ch) {
            case '*': o += "\xE2\x8B\x85"; break;  // · (centered dot)
            case '/': o += "\xC3\xB7";     break;  // ÷
            case '-': o += "\xE2\x88\x92"; break;  // −
            default:  o += ch;             break;
        }
    }
    return o;
}

// Run-mode CAS action bar: d/dx, ∫, exact(simplify), decimal(float) — a thin
// strip below the status row that exercises the sandboxed engine without
// disturbing the keypad grid. Labels are ASCII (the MSDF atlas has no ∫/≈ glyph).
const cas::Op kCasOps[4]    = {cas::Op::Derivative, cas::Op::Integral,
                               cas::Op::Simplify,   cas::Op::Numeric};
const char*   kCasLabels[4] = {"d/dx", "Int", "Exact", "Dec"};

int casBarRects(float W, float contentH, Rect out[4]) {
    float topH = contentH * 0.055f;
    float h = std::max(contentH * 0.05f, 30.0f);
    float y  = topH + contentH * 0.012f;
    float x0 = W * 0.03f, gap = W * 0.012f;
    float w  = (W * 0.94f - 3 * gap) / 4.0f;
    for (int i = 0; i < 4; i++) out[i] = {x0 + i * (w + gap), y, w, h};
    return 4;
}

// Submit the current Run expression to the sandboxed CAS engine. The reply lands
// asynchronously in the render loop's poll and is pushed to the history tape.
void casAction(AppState* st, cas::Op op) {
    std::string src = st->runCalc.editor().linearize();
    if (src.empty()) return;
    cas::Request req;
    req.op      = op;
    req.expr    = src;
    req.var     = "x";
    req.degrees = st->runCalc.degrees();
    st->casToken   = st->cas.submit(req);
    st->casPending = true;   // outstanding until the loop's poll delivers it
    const char* tag = op == cas::Op::Derivative ? "d/dx " :
                      op == cas::Op::Integral   ? "Int "  :
                      op == cas::Op::Numeric    ? "dec "  : "exact ";
    st->casPendingLabel  = std::string(tag) + casPretty(src);
    st->casPendingPrefix = tag;
    st->casPendingAst    = parseExprAst(src);   // 2D echo of what was submitted

    // Instant execution: consume the editor on tap. The expression is now "in
    // flight" to the CAS (result lands on the tape via the loop's poll), so there
    // is no invisible pending state and nothing left for the numeric '=' path to
    // trip over with an unbound variable.
    st->runCalc.input("C");
    st->runPreviewDirty = false;
    st->runPreviewCache.clear();
    st->dirty = true;
}

void keypadInput(AppState* st, const std::string& t) {
    if (st->mode == AppMode::Run) {       // docked keypad → the Calc facade
        if      (t == "done") runCommit(st);            // OK = "="
        else if (t == "x")    st->runCalc.input("x");   // the variable (CAS: d/dx, ∫, …)
        else                  st->runCalc.input(t);     // tokens map 1:1 to Calc
        if (t != "done") { st->runEditTime = nowSeconds(); st->runPreviewDirty = true; }
        st->dirty = true;
        return;
    }
    if (t == "done") { closeEqEditor(st); return; }
    if (t == "back")       st->editor.backspace();
    else if (t == "left")  st->editor.moveLeft();
    else if (t == "right") st->editor.moveRight();
    else if (t == "up")    st->editor.moveUp();
    else if (t == "down")  st->editor.moveDown();
    else if (t == "frac")    st->editor.insertFraction();
    else if (t == "sqrt")    st->editor.insertSqrt();
    else if (t == "nthroot") st->editor.insertNthRoot();
    else if (t == "pow")     st->editor.insertPower();
    else if (t == "x")       st->editor.insertAtom(activeVarLabel(st));  // x or t per type
    else st->editor.insertAtom(eqTokenText(t));
    syncSelectedExpr(st);
    st->dirty = true;
}

// ── Equation meta bar: a slim strip riding on top of the slide-up keypad with
//    the equation's TYPE (f(x) / r(θ) / x,y) and COLOUR — so both can be chosen
//    right where the equation is created/edited, not only from the list. ──
float eqMetaBarH(float contentH) { return contentH * 0.052f; }

struct EqMetaGeom { Rect bar; Rect type[3]; Rect swatch[kPaletteN]; };

EqMetaGeom eqMetaGeom(const Rect& panel, float contentH) {
    EqMetaGeom m;
    float h = eqMetaBarH(contentH);
    m.bar = {panel.x, panel.y - h, panel.w, h};
    float pad = h * 0.18f, ch = h - 2.0f * pad;
    float x = m.bar.x + pad * 2.0f;
    float tw = ch * 2.0f;   // narrow enough to keep clear air before the swatches
    for (int i = 0; i < 3; i++) {
        m.type[i] = {x, m.bar.y + pad, tw, ch};
        x += tw + pad;
    }
    float sw = ch * 0.92f, sy = m.bar.y + (h - sw) * 0.5f;
    float sx = m.bar.x + m.bar.w - pad * 2.0f - kPaletteN * (sw + pad) + pad;
    for (int i = 0; i < kPaletteN; i++) {
        m.swatch[i] = {sx, sy, sw, sw};
        sx += sw + pad;
    }
    return m;
}

const graphing::EqType kMetaTypes[3] = {
    graphing::EqType::Function, graphing::EqType::Polar, graphing::EqType::Parametric};

void drawEqMetaBar(Canvas& c, AppState* st, const Rect& panel) {
    if (st->selectedEq < 0 || st->selectedEq >= static_cast<int>(st->eqs.size())) return;
    const graphing::Equation& e = st->eqs[st->selectedEq];
    EqMetaGeom m = eqMetaGeom(panel, contentHeight(st));
    c.rect(m.bar.x, m.bar.y, m.bar.w, m.bar.h, col::panel2, 0.0f);
    for (int i = 0; i < 3; i++) {
        bool on = (e.type == kMetaTypes[i]);
        c.button(m.type[i].x, m.type[i].y, m.type[i].w, m.type[i].h,
                 eqTypeTag(kMetaTypes[i]), on ? col::accent : col::btnIdle,
                 on ? col::bg : col::dim, m.type[i].h * 0.24f);
    }
    for (int i = 0; i < kPaletteN; i++) {
        const Rect& s = m.swatch[i];
        const graphing::RGBA& p = kPalette[i];
        bool cur = std::fabs(p.r - e.color.r) + std::fabs(p.g - e.color.g) +
                   std::fabs(p.b - e.color.b) < 0.01f;
        if (cur)  // white ring marks the equation's current colour
            c.rect(s.x - 2.5f, s.y - 2.5f, s.w + 5.0f, s.h + 5.0f, col::text, s.w * 0.34f);
        c.rect(s.x, s.y, s.w, s.h, toColor(p), s.w * 0.28f);
    }
}

// Direct type set from the meta bar (vs the list's cycle button).
void setEqType(AppState* st, int i, graphing::EqType t) {
    if (i < 0 || i >= static_cast<int>(st->eqs.size()) || st->eqs[i].type == t) return;
    if (st->keypadOpen && st->selectedEq == i) syncSelectedExpr(st);
    st->eqs[i].type = t;
    if (st->keypadOpen && st->selectedEq == i) {
        st->editField = 0;
        loadFieldIntoEditor(st);
    }
    st->dirty = true;
}

bool handleEqMetaTap(AppState* st, const Rect& panel, float x, float y) {
    EqMetaGeom m = eqMetaGeom(panel, contentHeight(st));
    if (!m.bar.contains(x, y)) return false;
    for (int i = 0; i < 3; i++)
        if (m.type[i].contains(x, y)) { setEqType(st, st->selectedEq, kMetaTypes[i]); return true; }
    for (int i = 0; i < kPaletteN; i++)
        if (m.swatch[i].contains(x, y)) {
            if (st->selectedEq >= 0 && st->selectedEq < static_cast<int>(st->eqs.size())) {
                st->eqs[st->selectedEq].color = kPalette[i];
                st->dirty = true;
            }
            return true;
        }
    return true;  // taps on the bar's dead space don't close the keypad
}

// Parametric shows two field tabs (x(t)/y(t)) at the left of the keypad display;
// fills out[0..1] and returns the count (0 for Function/Polar — single field).
int keypadFieldTabs(const Rect& disp, const AppState* st, Rect out[2]) {
    if (eqFieldCount(activeType(st)) < 2) return 0;
    float tw = disp.w * 0.17f, gap = disp.h * 0.06f, th = (disp.h - gap) * 0.5f;
    float tx = disp.x + disp.w * 0.012f;
    out[0] = {tx, disp.y, tw, th};
    out[1] = {tx, disp.y + th + gap, tw, th};
    return 2;
}

void switchField(AppState* st, int field) {
    if (field == st->editField) return;
    syncSelectedExpr(st);          // persist what's in the editor now
    st->editField = field;
    loadFieldIntoEditor(st);       // bring in the other field
    st->dirty = true;
}

void drawKeypad(Canvas& c, AppState* st, const Rect& panel) {
    bool run = (st->mode == AppMode::Run);
    if (!run) drawEqMetaBar(c, st, panel);   // type + colour riding on the panel
    c.rect(panel.x, panel.y, panel.w, panel.h, col::panel, 0.0f);
    c.segment(panel.x, panel.y, panel.x + panel.w, panel.y,
              std::max(1.5f, panel.h * 0.006f), col::accent);
    Rect disp = keypadDispRect(panel);
    c.rect(disp.x, disp.y, disp.w, disp.h, col::bg, disp.h * 0.10f);
    float innerL = disp.x + disp.w * 0.04f;
    Rect tabs[2];
    if (!run && keypadFieldTabs(disp, st, tabs) == 2) {   // parametric: x(t)/y(t) tabs
        for (int k = 0; k < 2; k++) {
            bool on = (st->editField == k);
            c.rect(tabs[k].x, tabs[k].y, tabs[k].w, tabs[k].h,
                   on ? col::accent : col::panel2, tabs[k].h * 0.22f);
            c.textCentered(k == 0 ? "x(t)" : "y(t)", tabs[k].x + tabs[k].w * 0.5f,
                           tabs[k].y + tabs[k].h * 0.22f, tabs[k].h * 0.52f,
                           on ? col::bg : col::dim);
        }
        innerL = disp.x + disp.w * 0.21f;
    } else if (!run) {                    // function / polar: 𝑦ᵢ = / 𝑟ᵢ = ghost tag
        char var = activeType(st) == graphing::EqType::Polar ? 'r' : 'y';
        char lhs[2] = {var, '\0'};
        int sub = (st->selectedEq >= 0 && st->selectedEq < static_cast<int>(st->eqs.size()))
                      ? eqTypeIndex(st, st->eqs[st->selectedEq]) : 0;
        Rect tag{disp.x + disp.w * 0.02f, disp.y, disp.w * 0.12f, disp.h};
        mathlayout::drawGhostLhs(c, lhs, sub, tag, col::dim);
        innerL = disp.x + disp.w * 0.15f;
    }
    Rect inner{innerL, disp.y, disp.x + disp.w - innerL - disp.w * 0.01f, disp.h};
    if (run && st->runCalc.showingResult()) {  // show the committed result, right-aligned
        std::string res = st->runCalc.displayText();
        float rs = disp.h * 0.5f;
        c.setClip(inner.x, inner.y, inner.w, inner.h);
        c.text(res, inner.x + inner.w - std::min(inner.w, c.textWidth(res, rs)),
               inner.y + inner.h * 0.28f, rs, col::text);
        c.clearClip();
    } else {
        c.setClip(inner.x, inner.y, inner.w, inner.h);
        mathlayout::drawEditor(c, run ? st->runCalc.editor() : st->editor, inner);
        c.clearClip();
    }

    Key keys[36];
    keypadKeys(panel, keys);
    char varCh = run ? 'x' : activeVarLabel(st)[0];   // x, or t for polar/parametric
    for (int i = 0; i < 36; i++) {
        Color bg, fg;
        keypadColors(keys[i].token, bg, fg);
        std::string tok = keys[i].token;
        // LaTeX-style icons: the variable + template keys render the same math
        // typography they insert (italic var, surd + placeholder, stacked frac, …).
        bool hasIcon = true;
        mathlayout::KeyIcon icon{};
        if      (tok == "x")       icon = mathlayout::KeyIcon::Var;
        else if (tok == "frac")    icon = mathlayout::KeyIcon::Frac;
        else if (tok == "sqrt")    icon = mathlayout::KeyIcon::Sqrt;
        else if (tok == "nthroot") icon = mathlayout::KeyIcon::NthRoot;
        else if (tok == "pow")     icon = mathlayout::KeyIcon::Power;
        else hasIcon = false;
        if (hasIcon) {
            c.rect(keys[i].r.x, keys[i].r.y, keys[i].r.w, keys[i].r.h, bg, keys[i].r.h * 0.20f);
            mathlayout::drawKeyIcon(c, icon, keys[i].r, fg, varCh);
            continue;
        }
        const char* label = keys[i].label;
        if (tok == "done") label = run ? "=" : "OK";
        c.button(keys[i].r.x, keys[i].r.y, keys[i].r.w, keys[i].r.h, label,
                 bg, fg, keys[i].r.h * 0.20f);
    }
}

void handleKeypadTap(AppState* st, float x, float y) {
    float W = static_cast<float>(st->host.width());
    float anim = (st->mode == AppMode::Run) ? 1.0f : st->keypadAnim;
    Rect panel = keypadPanelRect(W, contentHeight(st), anim);
    if (st->mode != AppMode::Run) {  // parametric field tabs only when editing equations
        Rect tabs[2];
        int nt = keypadFieldTabs(keypadDispRect(panel), st, tabs);
        for (int k = 0; k < nt; k++)
            if (tabs[k].contains(x, y)) { switchField(st, k); return; }
    }
    Key keys[36];
    keypadKeys(panel, keys);
    // Forgiving hit-test: grow each key by the inter-key gutter (`bp` in keypadKeys)
    // so taps that land in the gaps still register. The grown cells tile EXACTLY
    // (they meet at the grid-cell boundary), so there's no overlap ambiguity — first
    // match wins. This removes the dead zones without making far misses register.
    float slop = panel.w * 0.025f * 0.30f;
    for (int i = 0; i < 36; i++) {
        const Rect& r = keys[i].r;
        if (x >= r.x - slop && x <= r.x + r.w + slop &&
            y >= r.y - slop && y <= r.y + r.h + slop) {
            if (!st->lastToolStylus) saf::haptic(st->app);  // no haptic for a stylus
            keypadInput(st, keys[i].token);
            return;
        }
    }
}

// ── Equation-list overlay ─────────────────────────────────────────────────────
struct EqListGeom {
    Rect panel, close, add;
    Rect tabBar, addSheet;               // tab strip + "new sheet" button
    Rect exportBtn, importBtn;           // workspace JSON export / import (footer)
    Rect rowsView;                       // clip rect for the scrollable equation rows
    Rect tab[16]; int64_t tabId[16]; int tabN = 0;
    Rect chip[16], type[16], expr[16], del[16];
    int  count = 0;
};

EqListGeom eqListGeom(AppState* st, float W, float contentH) {
    EqListGeom g;
    g.count = std::min(16, static_cast<int>(st->eqs.size()));
    float panelW = W * 0.88f, panelX = (W - panelW) * 0.5f;
    float pad   = panelW * 0.04f;
    float tabH  = contentH * 0.070f;
    float rowH  = contentH * 0.064f;     // tight rows → denser list
    float addH  = rowH * 0.92f, footH = rowH * 0.78f;

    // Dynamic height: the panel hugs its content, up to a cap past which the rows
    // scroll inside a fixed viewport.
    float rowsTotal = g.count * rowH;
    float rowsViewH = std::min(rowsTotal, contentH * 0.44f);
    float panelH = tabH + pad * 0.5f + rowsViewH + pad * 0.5f + addH + pad * 0.5f + footH + pad * 0.6f;
    panelH = std::min(panelH, contentH * 0.88f);
    float panelY = std::max(contentH * 0.06f, (contentH - panelH) * 0.5f);
    g.panel = {panelX, panelY, panelW, panelH};

    // Tab bar across the top: scrollable sheet tabs, then a "+" and a close "X".
    g.close    = {panelX + panelW - tabH * 0.82f, panelY + tabH * 0.16f, tabH * 0.62f, tabH * 0.62f};
    float addW = tabH * 0.86f;
    g.addSheet = {g.close.x - addW - pad * 0.3f, panelY + tabH * 0.16f, addW, tabH * 0.66f};
    g.tabBar   = {panelX + pad, panelY, g.addSheet.x - pad * 0.4f - (panelX + pad), tabH};
    g.tabN = std::min(16, static_cast<int>(st->sheets.size()));
    float tabW = g.tabBar.w * 0.42f, tgap = pad * 0.3f;
    float maxTab = std::max(0.0f, g.tabN * (tabW + tgap) - g.tabBar.w);
    st->tabScroll = std::min(std::max(0.0f, st->tabScroll), maxTab);
    for (int i = 0; i < g.tabN; i++) {
        g.tab[i]   = {g.tabBar.x - st->tabScroll + i * (tabW + tgap),
                      panelY + tabH * 0.16f, tabW, tabH * 0.66f};
        g.tabId[i] = st->sheets[i].id;
    }

    // Rows live in a clipped viewport and scroll vertically when they overflow.
    float rowsTop = panelY + tabH + pad * 0.5f;
    g.rowsView = {panelX + pad, rowsTop, panelW - 2 * pad, rowsViewH};
    st->eqScroll = std::min(std::max(0.0f, st->eqScroll), std::max(0.0f, rowsTotal - rowsViewH));
    for (int i = 0; i < g.count; i++) {
        float ry = rowsTop - st->eqScroll + i * rowH;
        float rx = panelX + pad, rw = panelW - 2 * pad;
        float chipS = rowH * 0.40f;
        g.chip[i] = {rx, ry + (rowH - chipS) * 0.5f, chipS, chipS};
        g.del[i]  = {rx + rw - rowH * 0.48f, ry + (rowH - rowH * 0.46f) * 0.5f, rowH * 0.46f, rowH * 0.46f};
        float tw = rowH * 1.15f, th = rowH * 0.5f;   // compact type badge
        g.type[i] = {g.chip[i].x + chipS + pad * 0.35f, ry + (rowH - th) * 0.5f, tw, th};
        float ex = g.type[i].x + tw + pad * 0.35f;
        g.expr[i] = {ex, ry, g.del[i].x - ex - pad * 0.3f, rowH};
    }

    // Add + export/import footer, just below the rows viewport (hugs content).
    float belowY = rowsTop + rowsViewH + pad * 0.5f;
    g.add = {panelX + pad, belowY, panelW - 2 * pad, addH};
    float fbw = (panelW - 2 * pad - pad) * 0.5f, footY = belowY + addH + pad * 0.5f;
    g.exportBtn = {panelX + pad, footY, fbw, footH};
    g.importBtn = {panelX + pad + fbw + pad, footY, fbw, footH};
    return g;
}

void drawEqList(Canvas& c, AppState* st, float W, float contentH) {
    c.rect(0, 0, W, contentH, Color{0.0f, 0.0f, 0.0f, 0.55f}, 0.0f);  // backdrop
    EqListGeom g = eqListGeom(st, W, contentH);
    c.rect(g.panel.x, g.panel.y, g.panel.w, g.panel.h, col::panel2, g.panel.h * 0.03f);

    // Sheet tab bar (scrollable, clipped). Active tab: subtle fill + blue underline.
    c.setClip(g.tabBar.x, g.tabBar.y, g.tabBar.w, g.tabBar.h);
    for (int i = 0; i < g.tabN; i++) {
        bool on = (g.tabId[i] == st->activeSheet);
        const Rect& t = g.tab[i];
        c.rect(t.x, t.y, t.w, t.h, on ? col::panel : col::panel2, t.h * 0.18f);
        std::string nm = ellipsize(c, st->sheets[i].name, t.h * 0.42f, t.w * 0.86f);
        c.textCentered(nm, t.x + t.w * 0.5f, t.y + t.h * 0.28f, t.h * 0.42f,
                       on ? col::text : col::dim);
        if (on)
            c.rect(t.x + t.w * 0.12f, t.y + t.h * 0.86f, t.w * 0.76f, t.h * 0.10f,
                   col::accent, t.h * 0.05f);
    }
    c.clearClip();
    c.button(g.addSheet.x, g.addSheet.y, g.addSheet.w, g.addSheet.h, "+",
             col::btnIdle, col::accent, g.addSheet.h * 0.28f);
    c.button(g.close.x, g.close.y, g.close.w, g.close.h, "X", col::btnIdle, col::text,
             g.close.h * 0.3f);

    // ONE label size for the whole list (the tightest row sets it) — mixed sizes
    // read as visual noise. Fallback (flat-text) rows return 0 and don't vote.
    float shared = 0.0f;
    for (int i = 0; i < g.count; i++) {
        float s = fitEqLabel2D(c, st->eqs[i], eqTypeIndex(st, st->eqs[i]), g.expr[i]);
        if (s > 0.0f) shared = (shared == 0.0f) ? s : std::min(shared, s);
    }

    c.setClip(g.rowsView.x, g.rowsView.y, g.rowsView.w, g.rowsView.h);
    for (int i = 0; i < g.count; i++) {
        const graphing::Equation& e = st->eqs[i];
        Color cc = toColor(e.color);
        if (!e.enabled) cc.a *= 0.32f;
        c.rect(g.chip[i].x, g.chip[i].y, g.chip[i].w, g.chip[i].h, cc, g.chip[i].w * 0.28f);
        c.button(g.type[i].x, g.type[i].y, g.type[i].w, g.type[i].h, eqTypeTag(e.type),
                 col::btnIdle, col::accent, g.type[i].h * 0.26f);
        // 2D natural-display label ("y = <expr>"), clipped to the row's expr cell
        // intersected with the rows viewport (Canvas holds a single clip rect).
        Rect ex = g.expr[i];
        float cy0 = std::max(ex.y, g.rowsView.y);
        float cy1 = std::min(ex.y + ex.h, g.rowsView.y + g.rowsView.h);
        if (cy1 > cy0) {
            c.setClip(ex.x, cy0, ex.w, cy1 - cy0);
            drawEqLabel2D(c, e, eqTypeIndex(st, e), ex,
                          e.enabled ? col::text : col::dim, shared);
            c.setClip(g.rowsView.x, g.rowsView.y, g.rowsView.w, g.rowsView.h);
        }
        c.button(g.del[i].x, g.del[i].y, g.del[i].w, g.del[i].h, "x",
                 Color{0.40f, 0.17f, 0.20f, 1.0f}, Color{1.0f, 0.78f, 0.78f, 1.0f},
                 g.del[i].h * 0.3f);
    }
    c.clearClip();
    c.button(g.add.x, g.add.y, g.add.w, g.add.h, "+ Add equation", col::btnIdle, col::green,
             g.add.h * 0.25f);
    c.button(g.exportBtn.x, g.exportBtn.y, g.exportBtn.w, g.exportBtn.h, "Export",
             col::panel2, col::accent, g.exportBtn.h * 0.24f);
    c.button(g.importBtn.x, g.importBtn.y, g.importBtn.w, g.importBtn.h, "Import",
             col::panel2, col::accent, g.importBtn.h * 0.24f);
}

void addEquation(AppState* st) {
    graphing::Equation e;
    e.id = st->nextEqId++;
    e.type = graphing::EqType::Function;
    e.color = kPalette[st->eqs.size() % kPaletteN];
    st->eqs.push_back(e);
}

void deleteEquation(AppState* st, int i) {
    if (i < 0 || i >= static_cast<int>(st->eqs.size())) return;
    st->eqs.erase(st->eqs.begin() + i);
    if (st->selectedEq >= static_cast<int>(st->eqs.size()))
        st->selectedEq = static_cast<int>(st->eqs.size()) - 1;
}

// Close the equation editor. An equation left completely empty (e.g. + Add
// equation followed straight by OK) is dropped — empty rows just bloat the list.
void closeEqEditor(AppState* st) {
    st->keypadOpen = false;
    int i = st->selectedEq;
    if (i >= 0 && i < static_cast<int>(st->eqs.size())) {
        const graphing::Equation& e = st->eqs[i];
        bool empty = e.type == graphing::EqType::Parametric
                         ? (e.exprX.empty() && e.exprY.empty())
                         : e.expr.empty();
        if (empty) deleteEquation(st, i);
    }
    st->dirty = true;
}

// Cycle Function → Polar → Parametric → Function (the list type selector).
void cycleEqType(AppState* st, int i) {
    if (i < 0 || i >= static_cast<int>(st->eqs.size())) return;
    graphing::EqType& t = st->eqs[i].type;
    t = (t == graphing::EqType::Function) ? graphing::EqType::Polar
      : (t == graphing::EqType::Polar)    ? graphing::EqType::Parametric
      :                                     graphing::EqType::Function;
    st->dirty = true;
}

// Load a sheet's data into live state (runtime: view window applied directly).
void applyWorkspace(AppState* st, const db::WorkspaceData& wd) {
    st->eqs        = wd.eqs;
    st->degrees    = wd.degrees;
    st->selectedEq = st->eqs.empty() ? 0
        : std::min(std::max(0, wd.selectedEq), static_cast<int>(st->eqs.size()) - 1);
    st->nextEqId   = wd.nextEqId > 0 ? wd.nextEqId : static_cast<int>(st->eqs.size()) + 1;
    plot::Viewport& v = st->plotview.viewport();
    v.xmin = wd.xmin; v.xmax = wd.xmax; v.ymin = wd.ymin; v.ymax = wd.ymax;
    st->trace.on = false;
    st->dirty = true;
}

// Persist the current sheet, then swap the active sheet to `id` and load it.
void switchSheet(AppState* st, int64_t id) {
    if (!st->persistent || id == st->activeSheet) return;
    st->store.saveSheet(st->activeSheet, snapshot(st));
    st->store.setActiveSheet(id);
    st->activeSheet = id;
    db::WorkspaceData wd;
    if (st->store.loadSheet(id, wd)) applyWorkspace(st, wd);
}

// Persist the current sheet, create a fresh empty one, and switch to it.
void newSheet(AppState* st) {
    if (!st->persistent) return;
    st->store.saveSheet(st->activeSheet, snapshot(st));
    int64_t id = st->store.createSheet("Sheet " + std::to_string(st->sheets.size() + 1));
    if (!id) return;
    st->store.setActiveSheet(id);
    st->activeSheet = id;
    st->eqs.clear();
    st->selectedEq = 0;
    st->nextEqId   = 1;
    st->plotview.viewport().reset(10.0);
    st->trace.on = false;
    st->store.saveSheet(id, snapshot(st));          // persist the empty sheet now
    int64_t active = 0;
    st->store.listSheets(st->sheets, active);       // refresh the tab list
    st->tabScroll = 1e9f;                           // reveal the new tab (clamped on layout)
    st->dirty = true;
}

// Persist the live sheet, then hand the whole workspace JSON to the SAF picker.
void triggerExport(AppState* st) {
    if (!st->persistent) return;
    st->store.saveSheetSync(st->activeSheet, snapshot(st));
    saf::requestExport(st->app, st->store.exportJson(), "workspace.json");
}

void handleEqListTap(AppState* st, float x, float y) {
    float W = static_cast<float>(st->host.width());
    EqListGeom g = eqListGeom(st, W, contentHeight(st));
    if (g.close.contains(x, y))     { st->listOpen = false; st->dirty = true; return; }
    if (g.addSheet.contains(x, y))  { newSheet(st); return; }
    if (g.exportBtn.contains(x, y)) { triggerExport(st); return; }
    if (g.importBtn.contains(x, y)) { saf::requestImport(st->app); return; }
    if (g.tabBar.contains(x, y)) {
        for (int i = 0; i < g.tabN; i++)
            if (g.tab[i].contains(x, y)) { switchSheet(st, g.tabId[i]); return; }
        return;  // empty tab-strip space — swallow
    }
    if (g.add.contains(x, y))   { addEquation(st); openKeypadFor(st, (int)st->eqs.size() - 1); return; }
    if (g.rowsView.contains(x, y)) {  // only rows inside the clipped viewport are tappable
        for (int i = 0; i < g.count; i++) {
            if (g.chip[i].contains(x, y)) { st->eqs[i].enabled = !st->eqs[i].enabled; st->dirty = true; return; }
            if (g.type[i].contains(x, y)) { cycleEqType(st, i); return; }
            if (g.del[i].contains(x, y))  { deleteEquation(st, i); st->dirty = true; return; }
            if (g.expr[i].contains(x, y)) { openKeypadFor(st, i); return; }
        }
        return;
    }
    if (!g.panel.contains(x, y)) { st->listOpen = false; st->dirty = true; }
}

// ── Sheet long-press menu (Rename / Delete) ─────────────────────────────────────
// A centered modal (not a popover): text composites in one pass over all rects,
// so an anchored popover would let list labels bleed through it. As a full modal
// with a backdrop (the list is skipped while open) it stays a clean layer.
struct SheetMenuGeom { Rect panel, rename, del, cancel; };

SheetMenuGeom sheetMenuGeom(float W, float contentH) {
    SheetMenuGeom m;
    float w = W * 0.64f, rh = contentH * 0.075f, titleH = contentH * 0.10f;
    float h = titleH + rh * 3.0f + rh * 0.9f;
    m.panel = {(W - w) * 0.5f, contentH * 0.28f, w, h};
    float pad = w * 0.06f, bw = w - 2 * pad, by = m.panel.y + titleH;
    m.rename = {m.panel.x + pad, by,               bw, rh};
    m.del    = {m.panel.x + pad, by + rh * 1.20f,  bw, rh};
    m.cancel = {m.panel.x + pad, by + rh * 2.55f,  bw, rh};
    return m;
}

void removeSheet(AppState* st, int64_t id) {
    if (!st->persistent || st->sheets.size() <= 1) { st->sheetMenuOpen = false; return; }
    st->store.deleteSheet(id);
    int64_t active = 0;
    st->store.listSheets(st->sheets, active);
    if (id == st->activeSheet && !st->sheets.empty()) {  // deleted the open sheet
        st->activeSheet = st->sheets.front().id;
        st->store.setActiveSheet(st->activeSheet);
        db::WorkspaceData wd;
        if (st->store.loadSheet(st->activeSheet, wd)) applyWorkspace(st, wd);
    }
    st->sheetMenuOpen = false;
    st->dirty = true;
}

void openRename(AppState* st, int64_t id) {
    st->renameSheetId = id;
    st->renameBuf.clear();
    for (const auto& s : st->sheets) if (s.id == id) { st->renameBuf = s.name; break; }
    st->renameShift = false;
    st->renameOpen    = true;
    st->sheetMenuOpen = false;
    st->dirty = true;
}

void drawSheetMenu(Canvas& c, AppState* st, float W, float contentH) {
    c.rect(0, 0, W, contentH, Color{0.0f, 0.0f, 0.0f, 0.55f}, 0.0f);  // backdrop
    SheetMenuGeom m = sheetMenuGeom(W, contentH);
    c.rect(m.panel.x, m.panel.y, m.panel.w, m.panel.h, col::panel, m.panel.h * 0.05f);

    std::string nm;
    for (const auto& s : st->sheets) if (s.id == st->menuSheetId) { nm = s.name; break; }
    float ts = contentH * 0.030f;
    c.text(ellipsize(c, nm, ts, m.panel.w * 0.88f),
           m.panel.x + m.panel.w * 0.06f, m.panel.y + contentH * 0.032f, ts, col::text);

    c.button(m.rename.x, m.rename.y, m.rename.w, m.rename.h, "Rename",
             col::btnIdle, col::text, m.rename.h * 0.22f);
    bool canDel = st->sheets.size() > 1;
    c.button(m.del.x, m.del.y, m.del.w, m.del.h, "Delete",
             col::btnIdle, canDel ? col::red : col::dim, m.del.h * 0.22f);
    c.button(m.cancel.x, m.cancel.y, m.cancel.w, m.cancel.h, "Cancel",
             col::panel2, col::dim, m.cancel.h * 0.22f);
}

void handleSheetMenuTap(AppState* st, float x, float y) {
    SheetMenuGeom m = sheetMenuGeom(static_cast<float>(st->host.width()), contentHeight(st));
    if (m.rename.contains(x, y)) { openRename(st, st->menuSheetId); return; }
    if (m.del.contains(x, y))    { removeSheet(st, st->menuSheetId); return; }
    st->sheetMenuOpen = false; st->dirty = true;  // Cancel or tap-out dismisses
}

// ── Rename keyboard (on-canvas; the math keypad has no letters) ──────────────────
const char* kNameRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};

struct RKey { Rect r; std::string label, token; };

int renameKeys(const Rect& panel, bool shift, RKey out[64]) {
    int n = 0;
    float pad = panel.w * 0.025f;
    float fieldH = panel.h * 0.17f;
    float gridTop = panel.y + fieldH + pad;
    float gridH = panel.h * 0.66f;
    float cw = (panel.w - 2 * pad) / 10.0f, ch = gridH / 5.0f, bp = cw * 0.06f;
    for (int r = 0; r < 4; r++) {
        std::string row = kNameRows[r];
        float rowW = row.size() * cw;
        float x0 = panel.x + (panel.w - rowW) * 0.5f;
        for (size_t i = 0; i < row.size(); i++) {
            std::string lab(1, row[i]);
            if (shift && row[i] >= 'a' && row[i] <= 'z') lab[0] = row[i] - 32;
            out[n++] = {{x0 + i * cw + bp, gridTop + r * ch + bp, cw - 2 * bp, ch - 2 * bp},
                        lab, lab};
        }
    }
    float ay = gridTop + 4 * ch + bp;
    float ax = panel.x + pad;
    out[n++] = {{ax, ay, cw * 1.4f, ch - 2 * bp}, "Shift", "shift"};  ax += cw * 1.5f;
    out[n++] = {{ax, ay, cw * 4.6f, ch - 2 * bp}, "Space", "space"};  ax += cw * 4.7f;
    out[n++] = {{ax, ay, cw * 2.6f, ch - 2 * bp}, "Back",  "back"};
    float by = panel.y + panel.h - panel.h * 0.13f, bw = (panel.w - 2 * pad - pad) * 0.5f;
    out[n++] = {{panel.x + pad, by, bw, panel.h * 0.11f}, "Cancel", "cancel"};
    out[n++] = {{panel.x + pad + bw + pad, by, bw, panel.h * 0.11f}, "OK", "ok"};
    return n;
}

Rect renamePanelRect(float W, float contentH) {
    float w = W * 0.92f, h = contentH * 0.56f;
    return {(W - w) * 0.5f, contentH * 0.20f, w, h};
}

void commitRename(AppState* st) {
    if (st->persistent && !st->renameBuf.empty())
        st->store.renameSheet(st->renameSheetId, st->renameBuf);
    int64_t active = 0;
    st->store.listSheets(st->sheets, active);
    st->renameOpen = false; st->dirty = true;
}

void drawRename(Canvas& c, AppState* st, float W, float contentH) {
    c.rect(0, 0, W, contentH, Color{0.0f, 0.0f, 0.0f, 0.62f}, 0.0f);  // backdrop
    Rect p = renamePanelRect(W, contentH);
    c.rect(p.x, p.y, p.w, p.h, col::panel, p.h * 0.03f);
    float pad = p.w * 0.025f, fieldH = p.h * 0.17f;
    Rect field{p.x + pad, p.y + pad, p.w - 2 * pad, fieldH - pad};
    c.rect(field.x, field.y, field.w, field.h, col::bg, field.h * 0.12f);
    float fs = field.h * 0.5f;
    std::string shown = ellipsize(c, st->renameBuf, fs, field.w * 0.9f);
    c.text(shown, field.x + field.w * 0.02f, field.y + field.h * 0.26f, fs, col::text);
    if (caretBlinkOn()) {  // blinking blue caret, snug to text
        float caretX = field.x + field.w * 0.02f + c.textWidth(shown, fs) + fs * 0.03f;
        c.rect(caretX, field.y + field.h * 0.22f, std::max(2.5f, fs * 0.07f), field.h * 0.56f, col::accent, 0.0f);
    }

    RKey keys[64];
    int nk = renameKeys(p, st->renameShift, keys);
    for (int i = 0; i < nk; i++) {
        Color bg = col::btnIdle, fg = col::text;
        if (keys[i].token == "shift" && st->renameShift) { bg = col::accent; fg = col::bg; }
        else if (keys[i].token == "ok")     { bg = col::accent; fg = Color{1, 1, 1, 1}; }
        else if (keys[i].token == "cancel") { fg = col::dim; }
        c.button(keys[i].r.x, keys[i].r.y, keys[i].r.w, keys[i].r.h, keys[i].label.c_str(),
                 bg, fg, keys[i].r.h * 0.20f);
    }
}

void handleRenameTap(AppState* st, float x, float y) {
    float W = static_cast<float>(st->host.width()), contentH = contentHeight(st);
    Rect p = renamePanelRect(W, contentH);
    RKey keys[64];
    int nk = renameKeys(p, st->renameShift, keys);
    for (int i = 0; i < nk; i++) {
        if (!keys[i].r.contains(x, y)) continue;
        const std::string& t = keys[i].token;
        if      (t == "shift")  st->renameShift = !st->renameShift;
        else if (t == "space")  { if (st->renameBuf.size() < 24) st->renameBuf += ' '; }
        else if (t == "back")   { if (!st->renameBuf.empty()) st->renameBuf.pop_back(); }
        else if (t == "cancel") st->renameOpen = false;
        else if (t == "ok")     commitRename(st);
        else if (st->renameBuf.size() < 24) st->renameBuf += keys[i].label;  // a char
        st->dirty = true;
        return;
    }
    if (!p.contains(x, y)) { st->renameOpen = false; st->dirty = true; }  // tap-out cancels
}

// ── Plot interaction ──────────────────────────────────────────────────────────
void resetView(AppState* st) {
    st->plotview.viewport().reset(10.0);
    st->trace.on = false;
    st->dirty = true;
}

// ── Trace: ONE shared cursor x, a marker per SELECTED curve. Which curves are
//    traced is an explicit selection (`traceIds`, toggled via the chip strip at
//    the top of the plot) — so tracing several at once, or exactly the one you
//    want, is a tap, not a fight with the nearest-curve heuristic. ──
bool eqTraced(const AppState* st, const graphing::Equation& e) {
    return std::find(st->traceIds.begin(), st->traceIds.end(), e.id) != st->traceIds.end();
}

// Traceable = an enabled y=f(x) with content (polar/parametric are t-indexed —
// a vertical-cursor trace doesn't apply to them).
bool eqTraceable(const graphing::Equation& e) {
    return e.enabled && e.type == graphing::EqType::Function && !e.expr.empty();
}

// Recompute the per-curve markers for the current cursor x (cheap; run per frame
// while tracing so edits/toggles/colour changes are always reflected).
void rebuildTraceMarkers(AppState* st) {
    st->traces.clear();
    if (!st->traceMode || !st->trace.on) return;
    const plot::Viewport& v = st->plotview.viewport();
    st->trace.wx = std::min(std::max(st->trace.wx, v.xmin), v.xmax);
    for (const auto& e : st->eqs) {
        if (!eqTraceable(e) || !eqTraced(st, e)) continue;
        plot::TraceMarker m;
        m.on = true;
        m.wx = st->trace.wx;
        double wy = 0.0;
        m.valid = graphing::evalFunctionAt(e.expr, st->trace.wx, st->degrees, wy);
        m.wy = m.valid ? wy : 0.0;
        m.color = toColor(e.color);
        st->traces.push_back(m);
    }
    if (st->traces.empty()) {                 // nothing selected → bare cursor line
        plot::TraceMarker m;
        m.on = true; m.wx = st->trace.wx; m.valid = false;
        st->traces.push_back(m);
    }
}

void updateTrace(AppState* st, float screenX) {
    const plot::Viewport& v = st->plotview.viewport();
    st->trace.wx = std::min(std::max(v.toWorldX(screenX), v.xmin), v.xmax);
    st->trace.on = true;
    st->dirty = true;                          // markers rebuilt in frame()
}

// Fine-step trace: nudge the shared cursor by 1/400 of the visible span —
// finger drags are the coarse moves, these buttons are the precise ones.
void nudgeTrace(AppState* st, int dir) {
    const plot::Viewport& v = st->plotview.viewport();
    double wx = st->trace.on ? st->trace.wx : 0.5 * (v.xmin + v.xmax);
    st->trace.wx = std::min(std::max(wx + dir * (v.xmax - v.xmin) / 400.0, v.xmin), v.xmax);
    st->trace.on = true;
    st->dirty = true;
}

// Selection chip strip along the top of the plot while tracing: one chip per
// traceable curve; tap toggles that curve's marker. Returns the count.
int traceChips(AppState* st, Rect out[16], int ids[16]) {
    int n = 0;
    float s = st->plotRect.w * 0.085f, pad = s * 0.30f;
    float x = st->plotRect.x + pad, y = st->plotRect.y + pad;
    for (const auto& e : st->eqs) {
        if (!eqTraceable(e) || n >= 16) continue;
        out[n] = {x, y, s, s};
        ids[n] = e.id;
        x += s + pad;
        ++n;
    }
    return n;
}

void handleToolbarTap(AppState* st, float x, float y) {
    float W = static_cast<float>(st->host.width());
    Key b[5];
    int n = toolbarButtons(W, contentHeight(st), st->mode, st->traceMode, b);
    for (int i = 0; i < n; i++) {
        if (!b[i].r.contains(x, y)) continue;
        std::string t = b[i].token;
        if (t == "eqns")       { st->listOpen = true; }
        else if (t == "trace") {
            st->traceMode = !st->traceMode;
            st->trace.on  = st->traceMode;
            if (st->traceMode) {           // default: trace every traceable curve
                st->traceIds.clear();
                for (const auto& e : st->eqs)
                    if (eqTraceable(e)) st->traceIds.push_back(e.id);
                const plot::Viewport& v = st->plotview.viewport();
                st->trace.wx = 0.5 * (v.xmin + v.xmax);
            } else {
                st->traces.clear();
            }
        }
        else if (t == "reset") resetView(st);
        else if (t == "stepl") nudgeTrace(st, -1);
        else if (t == "stepr") nudgeTrace(st, +1);
        else {
            float cx = st->plotRect.x + st->plotRect.w * 0.5f;
            float cy = st->plotRect.y + st->plotRect.h * 0.5f;
            st->plotview.viewport().zoomAbout(cx, cy, t == "zoomin" ? 0.8 : 1.25);
        }
        st->dirty = true;
        return;
    }
}

// ── Modes (selector + Table view) ───────────────────────────────────────────────
const char* modeName(AppMode m) {
    return m == AppMode::Graph ? "GRAPH" : m == AppMode::Table ? "TABLE" : "RUN";
}

Rect modePillRect(float W, float topH) {
    float h = topH * 0.62f;
    return {W * 0.018f, (topH - h) * 0.5f, W * 0.16f, h};
}

// RAD/DEG toggle, top-right of the status strip (tappable in every mode — the
// angle setting drives graph/table sampling AND the RUN evaluator).
Rect anglePillRect(float W, float topH) {
    float h = topH * 0.62f, w = h * 2.1f;
    return {W * 0.972f - w, (topH - h) * 0.5f, w, h};
}

void cycleMode(AppState* st) {
    st->mode = st->mode == AppMode::Graph ? AppMode::Table
             : st->mode == AppMode::Table ? AppMode::Run
             :                              AppMode::Graph;
    // tableInit is seeded once (from the view) then owned by Setup — don't reset.
    st->trace.on = false;
    st->dirty = true;
}

// Smallest "nice" step (1/2/5 × 10ⁿ) giving ≈`target` rows over `range`.
double niceStep(double range, int target) {
    if (range <= 0.0 || target < 1) return 1.0;
    double raw = range / target;
    double mag = std::pow(10.0, std::floor(std::log10(raw)));
    double n   = raw / mag;
    double nice = n <= 1.0 ? 1.0 : n <= 2.0 ? 2.0 : n <= 5.0 ? 5.0 : 10.0;
    return nice * mag;
}

// Table cell: keep columns narrow. Drop to fewer sig-figs past 8 chars, then
// compact the exponent ("1.55e+09" → "1.55e9") so huge/tiny values stay short.
std::string fmtNum(double v) {
    if (!std::isfinite(v)) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    std::string s = buf;
    if (s.size() > 8) { std::snprintf(buf, sizeof buf, "%.3g", v); s = buf; }
    size_t e = s.find('e');
    if (e != std::string::npos) {
        std::string mant = s.substr(0, e + 1), exp = s.substr(e + 1);
        bool neg = !exp.empty() && exp[0] == '-';
        if (!exp.empty() && (exp[0] == '+' || exp[0] == '-')) exp.erase(0, 1);
        size_t z = exp.find_first_not_of('0');
        exp = (z == std::string::npos) ? std::string("0") : exp.substr(z);
        s = mant + (neg ? "-" : "") + exp;
    }
    return s;
}

// Plain value for editing in the Setup fields (no forced scientific).
std::string fmtPlain(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// Bounded x / f(x) value table (Start → End by Step) for the enabled functions.
void drawTable(Canvas& c, AppState* st, float topH, float botH, float W, float contentH) {
    Rect area = {0.0f, topH, W, contentH - topH - botH};

    if (!st->tableInit) {  // seed the domain from the current graph window
        const plot::Viewport& v = st->plotview.viewport();
        st->tableStep   = niceStep(v.xmax - v.xmin, 14);
        st->tableStart  = std::round(v.xmin / st->tableStep) * st->tableStep;
        st->tableEnd    = std::round(v.xmax / st->tableStep) * st->tableStep;
        st->tableScroll = 0.0f;
        st->tableInit   = true;
    }

    const graphing::Equation* fns[4];
    int nf = 0, hidden = 0;
    for (const auto& e : st->eqs) {
        if (!e.enabled) continue;
        if (e.type == graphing::EqType::Function && !e.expr.empty()) {
            if (nf < 4) fns[nf++] = &e;
        } else if (e.type != graphing::EqType::Function) {
            hidden++;  // polar/parametric can't be x-indexed
        }
    }
    int   cols = nf + 1;
    float colW = area.w / static_cast<float>(cols);

    // One harmonized type scale for the whole table: header == cell.
    float rowH = contentH * 0.052f;
    float ts   = rowH * 0.42f;
    float footerH = (hidden > 0) ? rowH * 0.62f : 0.0f;  // reserved for the hidden-note

    // Bounded row count (descending allowed if step<0 and end<start).
    long total = 0;
    if (st->tableStep != 0.0 && std::isfinite(st->tableStep)) {
        double span = (st->tableEnd - st->tableStart) / st->tableStep;
        if (span >= 0.0) total = static_cast<long>(std::floor(span + 1e-9)) + 1;
    }
    if (total < 1) total = 1;
    if (total > 100000) total = 100000;  // sanity cap

    Rect head = {area.x, area.y, area.w, rowH};
    Rect rows = {area.x, area.y + rowH, area.w, area.h - rowH - footerH};

    // Clamp vertical scroll to the bounded content.
    float contentRows = total * rowH;
    st->tableScroll = std::min(std::max(0.0f, st->tableScroll),
                               std::max(0.0f, contentRows - rows.h));

    long firstIdx = static_cast<long>(std::floor(st->tableScroll / rowH));
    if (firstIdx < 0) firstIdx = 0;
    int  nVis = static_cast<int>(rows.h / rowH) + 2;
    long lastIdx = std::min(total - 1, firstIdx + nVis);
    int  count = static_cast<int>(lastIdx - firstIdx + 1);
    if (count > 0)
        for (int k = 0; k < nf; k++)  // parse once per column → eval the visible window
            graphing::sampleEquation(*fns[k],
                st->tableStart + firstIdx * st->tableStep,
                st->tableStart + lastIdx  * st->tableStep,
                std::max(count, 2), st->degrees, st->tableColBuf[k]);

    // Header: math-italic names matching the equation list (𝑥, 𝑦₁, 𝑦₂ …).
    c.rect(head.x, head.y, head.w, head.h, col::panel, 0.0f);
    mathlayout::drawVarName(c, 'x', 0, Rect{area.x, head.y, colW, rowH}, col::text);
    for (int k = 0; k < nf; k++)
        mathlayout::drawVarName(c, 'y', eqTypeIndex(st, *fns[k]),
                                Rect{area.x + colW * (k + 1), head.y, colW, rowH},
                                toColor(fns[k]->color));
    c.segment(area.x, head.y + rowH, area.x + area.w, head.y + rowH,
              std::max(1.0f, rowH * 0.04f), col::track);

    // Rows (clipped, bounded).
    c.setClip(rows.x, rows.y, rows.w, rows.h);
    for (long idx = firstIdx; idx <= lastIdx; idx++) {
        int   s  = static_cast<int>(idx - firstIdx);
        float ry = rows.y + static_cast<float>(idx * rowH - st->tableScroll);
        if (idx & 1L) c.rect(rows.x, ry, rows.w, rowH, col::panel2, 0.0f);  // zebra
        double x = st->tableStart + static_cast<double>(idx) * st->tableStep;
        c.text(fmtNum(x), area.x + colW * 0.08f, ry + rowH * 0.32f, ts, col::dim);
        for (int k = 0; k < nf; k++) {
            const auto& pts = st->tableColBuf[k];
            std::string cell = (s < static_cast<int>(pts.size()) && pts[s].valid)
                             ? fmtNum(pts[s].wy) : "-";
            c.text(cell, area.x + colW * (k + 1) + colW * 0.08f, ry + rowH * 0.32f, ts, col::text);
        }
    }
    c.clearClip();

    for (int k = 1; k < cols; k++)  // column separators
        c.segment(area.x + colW * k, area.y, area.x + colW * k, area.y + area.h,
                  std::max(1.0f, area.w * 0.0008f), col::track);

    // Empty / discoverability state.
    if (nf == 0) {
        c.textCentered("No y = f(x) equations to tabulate.",
                       area.x + area.w * 0.5f, area.y + area.h * 0.42f, ts * 1.05f, col::text);
        c.textCentered(hidden > 0 ? "Polar / parametric graphs aren't tabulated — add a function in Eqns."
                                  : "Add a function in Eqns.",
                       area.x + area.w * 0.5f, area.y + area.h * 0.42f + rowH * 0.7f, ts * 0.85f, col::dim);
    } else if (hidden > 0) {  // reserved footer note (rows already exclude this strip)
        std::string note = std::to_string(hidden) + " polar/parametric not tabulated";
        float ns = ts * 0.72f, fy = area.y + area.h - footerH;
        c.rect(area.x, fy, area.w, footerH, col::panel, 0.0f);
        c.text(note, area.x + area.w * 0.5f - c.textWidth(note, ns) * 0.5f,
               fy + footerH * 0.30f, ns, col::dim);
    }
}

// Run / Calc: a result tape above the docked keypad (which holds the live input).
void drawRun(Canvas& c, AppState* st, float topH, float W, float contentH) {
    float kpTop = keypadPanelRect(W, contentH, 1.0f).y;
    float pad = W * 0.04f;

    // CAS action bar: route the current expression through the sandboxed engine.
    // Blue text means ready; dimmed means a computation is still in flight.
    Rect cbar[4]; casBarRects(W, contentH, cbar);
    const Color kCasBg = {0.267f, 0.267f, 0.267f, 1.0f};   // #444444, matches kFunc
    const Color kCasFg = {0.290f, 0.565f, 0.886f, 1.0f};   // #4A90E2 accent
    bool busy = st->casPending;
    for (int i = 0; i < 4; i++) {
        Color fg = busy ? col::dim : kCasFg;
        if (i == 0 || i == 1) {   // d/dx as a real fraction; ∫ from the math face
            c.rect(cbar[i].x, cbar[i].y, cbar[i].w, cbar[i].h, kCasBg, cbar[i].h * 0.24f);
            mathlayout::drawKeyIcon(c, i == 0 ? mathlayout::KeyIcon::Derivative
                                              : mathlayout::KeyIcon::Integral,
                                    cbar[i], fg, 'x');
        } else {
            c.button(cbar[i].x, cbar[i].y, cbar[i].w, cbar[i].h, kCasLabels[i],
                     kCasBg, fg, cbar[i].h * 0.24f);
        }
    }

    float areaTop = cbar[0].y + cbar[0].h + contentH * 0.012f;
    Rect area = {0.0f, areaTop, W, kpTop - areaTop};

    // Debounced live preview: re-evaluate only after ~120ms of idle typing.
    if (st->runPreviewDirty && nowSeconds() - st->runEditTime >= 0.12) {
        st->runPreviewCache = st->runCalc.preview();
        st->runPreviewDirty = false;
    }
    std::string prev = st->runCalc.showingResult() ? std::string() : st->runPreviewCache;
    float prevH = area.h * 0.18f;
    Rect hist = {area.x, area.y, area.w, area.h - prevH};

    // History tape: newest at the bottom, drag to scroll older rows up. Rows are
    // VARIABLE height — a 2D fraction stands ~2× a text line — so each row is
    // sized to its content and the gray label is stacked clear ABOVE the result
    // (measuring the result's above-baseline extent so they never collide).
    int   n    = static_cast<int>(st->runHistory.size());
    float unit = std::max(contentH * 0.085f, hist.h * 0.30f);  // sizing reference
    float es   = unit * 0.26f;          // gray label font
    float rs   = unit * 0.40f;          // 1D result font
    float maxRes = unit * 0.52f;        // 2D result target font (shrinks to fit width)
    float maxW = hist.w - 2 * pad;
    float labelH = es * 1.25f, gap = unit * 0.12f, padB = unit * 0.20f;
    float esMax  = es * 1.55f;          // 2D echo target size (fractions grow rows)

    // The gray source echo renders 2D when its input parsed (labelAst) — real
    // fractions and stretchy parens instead of "(1)÷(4)". Height/width of that
    // block (prefix like "d/dx " stays flat text ahead of the math).
    auto labelBlock = [&](const TapeEntry& te, float& lh, float& lw) {
        if (te.labelAst) {
            float pw = te.labelPrefix.empty() ? 0.0f : c.textWidth(te.labelPrefix, es);
            mathlayout::AstFit f =
                mathlayout::fitAst(c, *te.labelAst, esMax, std::max(1.0f, maxW - pw));
            lh = f.above + f.below;
            lw = pw + f.width;
        } else {
            lh = labelH;
            lw = c.textWidth(te.label, es);
        }
    };

    auto rowHeight = [&](const TapeEntry& te) -> float {       // no allocation
        float lh, lw;
        labelBlock(te, lh, lw);
        float resH = rs * 1.25f;
        if (te.ast) {
            mathlayout::AstFit f = mathlayout::fitAst(c, *te.ast, maxRes, maxW);
            resH = f.above + f.below;
        }
        return lh + gap + resH + padB;
    };

    float total = 0.0f, maxOverflow = 0.0f;   // widest entry's spill past the view, at the floor size
    for (const auto& te : st->runHistory) {
        total += rowHeight(te);
        if (te.ast) {
            mathlayout::AstFit f = mathlayout::fitAst(c, *te.ast, maxRes, maxW);
            maxOverflow = std::max(maxOverflow, f.width - maxW);
        } else {
            // 1D fallback result (didn't parse to an AST): count ITS width too, or a
            // long unparsed result would overflow with the scroll clamp locked at 0.
            float rw = c.textWidth("= ", rs) + c.textWidth(te.resultRaw, rs);
            maxOverflow = std::max(maxOverflow, rw - maxW);
        }
        float lh, lw;
        labelBlock(te, lh, lw);
        maxOverflow = std::max(maxOverflow, lw - maxW);
    }
    st->runScroll  = std::min(std::max(0.0f, st->runScroll), std::max(0.0f, total - hist.h));
    st->runScrollX = std::min(std::max(0.0f, st->runScrollX), std::max(0.0f, maxOverflow));

    // Right-edge X for content of width `cw`: right-aligned when it fits; when it's
    // still wider than the view at the legibility floor, left-anchor it and pan by
    // this entry's share of the horizontal scroll (so you can read off the right end).
    auto rightEdge = [&](float cw) -> float {
        float ov = cw - maxW;
        if (ov > 0.0f) return hist.x + pad + cw - std::min(st->runScrollX, ov);
        return hist.x + hist.w - pad;
    };

    c.setClip(hist.x, hist.y, hist.w, hist.h);
    float yb = hist.y + hist.h + st->runScroll;   // bottom edge of the newest row
    for (int i = n - 1; i >= 0; i--) {
        const TapeEntry& te = st->runHistory[i];
        float h   = rowHeight(te);
        float top = yb - h;
        if (yb >= hist.y && top <= hist.y + hist.h) {          // row is visible
            float lh, lw;
            labelBlock(te, lh, lw);
            if (te.labelAst) {                                 // 2D gray source echo
                float pw = te.labelPrefix.empty() ? 0.0f : c.textWidth(te.labelPrefix, es);
                mathlayout::AstFit f =
                    mathlayout::fitAst(c, *te.labelAst, esMax, std::max(1.0f, maxW - pw));
                float rx = rightEdge(lw);
                float yAxis = top + f.above;
                if (pw > 0.0f)
                    mathbox::axisText(c, te.labelPrefix, rx - lw, yAxis, es, col::dim);
                mathlayout::drawAstAt(c, *te.labelAst, rx, yAxis, f.size, &col::dim);
            } else {
                c.text(te.label, rightEdge(lw) - lw, top + es * 0.15f, es, col::dim);
            }
            float resTop = top + lh + gap;
            if (te.ast) {
                mathlayout::AstFit f = mathlayout::fitAst(c, *te.ast, maxRes, maxW);
                mathlayout::drawAstAt(c, *te.ast, rightEdge(f.width), resTop + f.above, f.size);
            } else {
                std::string rr = "= " + casPretty(te.resultRaw);
                float rw = c.textWidth(rr, rs);
                c.text(rr, rightEdge(rw) - rw, resTop, rs, col::text);
            }
            c.segment(hist.x + pad, yb - 1.0f, hist.x + hist.w - pad, yb - 1.0f, 1.0f, col::track);
        }
        yb = top;
        if (yb < hist.y) break;        // everything older is above the viewport
    }
    c.clearClip();

    // Horizontal scroll affordance: shown only when an entry overflows the view.
    // The thumb position tracks runScrollX, so the equation reads as draggable.
    if (maxOverflow > 1.0f) {
        float th = std::max(2.0f, unit * 0.035f);
        float trackY = hist.y + hist.h - th - 1.0f;
        float trackW = hist.w - 2 * pad;
        float frac   = maxW / (maxW + maxOverflow);              // visible portion
        float thumbW = std::max(trackW * frac, unit * 0.18f);
        float t      = std::min(st->runScrollX / maxOverflow, 1.0f);
        c.rect(hist.x + pad, trackY, trackW, th, col::track);
        c.rect(hist.x + pad + (trackW - thumbW) * t, trackY, thumbW, th, col::dim);
    }

    if (!prev.empty()) {  // live preview of the current (uncommitted) input
        std::string pv = "= " + prev;
        float ps = prevH * 0.46f;
        c.text(pv, area.x + area.w - pad - c.textWidth(pv, ps),
               area.y + area.h - prevH + prevH * 0.28f, ps, col::accent);
    } else if (n == 0) {
        c.textCentered("Type an expression, then press =",
                       area.x + area.w * 0.5f, area.y + area.h * 0.45f, area.h * 0.05f, col::dim);
    }
}

// ── Table Setup modal (Start x / Step Δx) ───────────────────────────────────────
Rect tableSetupBtnRect(float W, float topH) {
    Rect mp = modePillRect(W, topH);
    float h = topH * 0.62f;
    return {mp.x + mp.w + topH * 0.25f, (topH - h) * 0.5f, topH * 2.4f, h};
}

Rect tableSetupPanel(float W, float contentH) {
    float w = W * 0.80f, h = contentH * 0.60f;
    return {(W - w) * 0.5f, contentH * 0.18f, w, h};
}

// Three field selectors (Start/End/Step) + a numeric pad. RKey shared with rename.
int tableSetupKeys(const Rect& p, RKey out[24]) {
    int n = 0;
    float pad = p.w * 0.045f, fieldH = p.h * 0.092f;
    float fy0 = p.y + p.h * 0.11f;
    for (int f = 0; f < 3; f++)
        out[n++] = {{p.x + pad, fy0 + f * (fieldH + pad * 0.32f), p.w - 2 * pad, fieldH},
                    "", f == 0 ? "field0" : f == 1 ? "field1" : "field2"};
    float gy = fy0 + 3 * (fieldH + pad * 0.32f) + pad * 0.3f;
    float gw = p.w - 2 * pad, cw = gw / 3.0f, gh = p.h * 0.33f, ch = gh / 4.0f, bp = cw * 0.06f;
    static const char* g[12] = {"7","8","9","4","5","6","1","2","3","\xC2\xB1","0","."};
    static const char* tk[12]= {"7","8","9","4","5","6","1","2","3","neg","0","dot"};
    for (int i = 0; i < 12; i++) {
        int r = i / 3, cc = i % 3;
        out[n++] = {{p.x + pad + cc * cw + bp, gy + r * ch + bp, cw - 2 * bp, ch - 2 * bp}, g[i], tk[i]};
    }
    out[n++] = {{p.x + pad, gy + gh + pad * 0.25f, gw, ch * 0.8f}, "Back", "back"};
    float by = p.y + p.h - p.h * 0.115f, bw = (gw - pad) * 0.5f;
    out[n++] = {{p.x + pad, by, bw, p.h * 0.10f}, "Cancel", "cancel"};
    out[n++] = {{p.x + pad + bw + pad, by, bw, p.h * 0.10f}, "OK", "ok"};
    return n;
}

void openTableSetup(AppState* st) {
    st->setupBuf[0] = fmtPlain(st->tableStart);
    st->setupBuf[1] = fmtPlain(st->tableEnd);
    st->setupBuf[2] = fmtPlain(st->tableStep);
    st->setupField = 0;
    st->tableSetupOpen = true;
    st->dirty = true;
}

void commitTableSetup(AppState* st) {
    double s  = std::atof(st->setupBuf[0].c_str());
    double e  = std::atof(st->setupBuf[1].c_str());
    double dx = std::atof(st->setupBuf[2].c_str());
    st->tableStart = s;
    st->tableEnd   = e;
    if (dx != 0.0 && std::isfinite(dx)) st->tableStep = dx;  // reject 0/NaN, keep old
    st->tableScroll = 0.0f;
    st->tableInit = true;
    st->tableSetupOpen = false;
    st->dirty = true;
}

void drawTableSetup(Canvas& c, AppState* st, float W, float contentH) {
    c.rect(0, 0, W, contentH, Color{0.0f, 0.0f, 0.0f, 0.62f}, 0.0f);  // backdrop
    Rect p = tableSetupPanel(W, contentH);
    c.rect(p.x, p.y, p.w, p.h, col::panel, p.h * 0.04f);
    c.text("Table Setup", p.x + p.w * 0.05f, p.y + p.h * 0.04f, p.h * 0.052f, col::text);

    RKey keys[24];
    int nk = tableSetupKeys(p, keys);
    const char* flabel[3] = {"Start x", "End x", "Step"};
    for (int i = 0; i < nk; i++) {
        const std::string& t = keys[i].token;
        const Rect& r = keys[i].r;
        if (t.rfind("field", 0) == 0) {
            int fi = t == "field0" ? 0 : t == "field1" ? 1 : 2;
            bool on = (st->setupField == fi);
            c.rect(r.x, r.y, r.w, r.h, col::bg, r.h * 0.14f);
            if (on) c.rect(r.x, r.y, 4.0f, r.h, col::accent, 0.0f);  // active marker
            float fs = r.h * 0.46f;
            c.text(flabel[fi], r.x + r.w * 0.03f, r.y + r.h * 0.30f, fs, col::dim);
            const std::string& val = st->setupBuf[fi];
            float vx = r.x + r.w * 0.34f;
            c.text(val, vx, r.y + r.h * 0.30f, fs, on ? col::accent : col::text);
            if (on && caretBlinkOn()) {  // distinct blinking blue caret, snug to text
                float cx = vx + c.textWidth(val, fs) + fs * 0.03f;
                c.rect(cx, r.y + r.h * 0.24f, std::max(2.5f, fs * 0.07f), r.h * 0.52f, col::accent, 0.0f);
            }
        } else {
            Color bg = col::btnIdle, fg = col::text;
            if (t == "ok")          { bg = col::accent; fg = Color{1, 1, 1, 1}; }
            else if (t == "cancel") { fg = col::dim; }
            c.button(r.x, r.y, r.w, r.h, keys[i].label.c_str(), bg, fg, r.h * 0.20f);
        }
    }
}

void handleTableSetupTap(AppState* st, float x, float y) {
    Rect p = tableSetupPanel(static_cast<float>(st->host.width()), contentHeight(st));
    RKey keys[24];
    int nk = tableSetupKeys(p, keys);
    std::string& buf = st->setupBuf[st->setupField];
    for (int i = 0; i < nk; i++) {
        if (!keys[i].r.contains(x, y)) continue;
        const std::string& t = keys[i].token;
        if      (t == "field0") st->setupField = 0;
        else if (t == "field1") st->setupField = 1;
        else if (t == "field2") st->setupField = 2;
        else if (t == "cancel") st->tableSetupOpen = false;
        else if (t == "ok")     commitTableSetup(st);
        else if (t == "back")   { if (!buf.empty()) buf.pop_back(); }
        else if (t == "neg")    { if (!buf.empty() && buf[0] == '-') buf.erase(0, 1); else buf.insert(buf.begin(), '-'); }
        else if (t == "dot")    { if (buf.find('.') == std::string::npos && buf.size() < 14) buf += '.'; }
        else if (buf.size() < 14) buf += t;  // a digit
        st->dirty = true;
        return;
    }
    if (!p.contains(x, y)) { st->tableSetupOpen = false; st->dirty = true; }  // tap-out cancels
}

void draw(AppState* st) {
    if (!st->host.ready()) return;
    const float W = static_cast<float>(st->host.width());
    const float contentH = contentHeight(st);

    std::vector<float> curves;
    std::vector<float> quads;
    Canvas c(curves, st->host.width(), st->host.height(),
             st->fontLoaded ? &st->font : nullptr, 0, st->navBarPx, 0, 0);
    if (const MsdfFont* mf = st->host.msdfFont()) c.useMsdf(mf, &quads);

    const float topH = contentH * 0.055f;
    const float botH = toolbarHeight(contentH);
    st->plotRect = {0.0f, topH, W, contentH - topH - botH};
    st->plotview.viewport().setScreenRect(st->plotRect.x, st->plotRect.y,
                                          st->plotRect.w, st->plotRect.h);
    if (!st->viewInit) {
        plot::Viewport& v = st->plotview.viewport();
        if (st->haveRestore) {  // restored window from SQLite (apply once)
            v.xmin = st->rXmin; v.xmax = st->rXmax;
            v.ymin = st->rYmin; v.ymax = st->rYmax;
            st->haveRestore = false;
        } else {
            v.reset(10.0);
        }
        st->viewInit = true;
    }

    c.clear(col::bg);

    if (st->mode == AppMode::Graph) {
        const plot::Viewport& vp = st->plotview.viewport();
        int sampleCount = std::max(2, std::min(760, static_cast<int>(vp.sw / 1.5f)));
        st->curveBufs.resize(st->eqs.size());
        st->curves.clear();
        for (size_t i = 0; i < st->eqs.size(); i++) {
            const graphing::Equation& eq = st->eqs[i];
            std::vector<plot::CurvePoint>& cb = st->curveBufs[i];
            cb.clear();
            if (!eqRenderable(eq)) continue;
            graphing::sampleEquation(eq, vp.xmin, vp.xmax, sampleCount, st->degrees, st->sampleBuf);
            cb.reserve(st->sampleBuf.size());
            for (const auto& s : st->sampleBuf) cb.push_back({s.wx, s.wy, s.valid});
            st->curves.push_back({cb.data(), static_cast<int>(cb.size()), toColor(eq.color)});
        }
        st->plotview.drawLabels = true;  // modals now cull underlying text via c.occlude()

        // Keep the plot's (later-pass) text from bleeding through the slide-up
        // keypad: clip plot emission to the strip above the panel (and its meta
        // bar) while it shows.
        if (st->keypadAnim > 0.001f) {
            float panelTop = keypadPanelRect(W, contentH, st->keypadAnim).y
                           - eqMetaBarH(contentH);
            st->plotview.setClipRect(st->plotRect.x, st->plotRect.y, st->plotRect.w,
                                     std::max(0.0f, panelTop - st->plotRect.y));
        } else {
            st->plotview.clearClipRect();
        }
        if (st->traceMode) rebuildTraceMarkers(st);
        st->plotview.draw(c, st->curves.data(), static_cast<int>(st->curves.size()),
                          st->traces.data(), static_cast<int>(st->traces.size()));

        // Trace-selection chips: one 𝑦ᵢ chip per traceable curve; filled = traced.
        if (st->traceMode && st->keypadAnim <= 0.001f) {
            Rect chips[16]; int ids[16];
            int n = traceChips(st, chips, ids);
            for (int i = 0; i < n; i++) {
                const graphing::Equation* e = nullptr;
                for (const auto& q : st->eqs) if (q.id == ids[i]) { e = &q; break; }
                if (!e) continue;
                bool on = eqTraced(st, *e);
                Color cc = toColor(e->color);
                if (!on) cc.a *= 0.30f;
                c.rect(chips[i].x, chips[i].y, chips[i].w, chips[i].h, cc,
                       chips[i].w * 0.22f);
                mathlayout::drawVarName(c, 'y', eqTypeIndex(st, *e), chips[i],
                                        on ? col::bg : col::dim);
            }
        }
    } else if (st->mode == AppMode::Table) {
        drawTable(c, st, topH, botH, W, contentH);  // modal (if any) occludes it cleanly
    } else {
        drawRun(c, st, topH, W, contentH);
    }

    // Status strip.
    c.rect(0, 0, W, topH, col::panel, 0.0f);
    c.segment(0, topH, W, topH, std::max(1.0f, topH * 0.03f), col::track);

    // Mode selector (left) — tap to cycle Graph → Table → Run. Same button style
    // as the Setup button so the top-bar controls share one type scale.
    Rect mp = modePillRect(W, topH);
    c.button(mp.x, mp.y, mp.w, mp.h, modeName(st->mode), col::panel2, col::accent, mp.h * 0.3f);

    // Angle toggle (right) — a real button: tap flips RAD ↔ DEG everywhere
    // (graph sampling, table, RUN trig). No equation title here: the Eqns
    // overlay already lists every equation, so a "first equation" banner was
    // redundant noise.
    Rect ap = anglePillRect(W, topH);
    c.button(ap.x, ap.y, ap.w, ap.h, st->degrees ? "DEG" : "RAD",
             col::panel2, col::text, ap.h * 0.5f);

    if (st->mode == AppMode::Table) {  // Table is multi-column → Setup lives here
        Rect sb = tableSetupBtnRect(W, topH);
        c.button(sb.x, sb.y, sb.w, sb.h, "Setup", col::btnIdle, col::accent, sb.h * 0.30f);
    }

    // Docked keypad in Run mode (always shown); slide-up keypad for equation edit.
    float kpA = (st->mode == AppMode::Run) ? 1.0f : st->keypadAnim;

    // Bottom toolbar — hidden while any keypad is up.
    if (kpA <= 0.001f) {
        c.segment(0, contentH - botH, W, contentH - botH, std::max(1.0f, botH * 0.02f), col::track);
        Key b[5];
        int n = toolbarButtons(W, contentH, st->mode, st->traceMode, b);
        for (int i = 0; i < n; i++) {
            bool active = (std::string(b[i].token) == "trace") && st->traceMode;
            c.button(b[i].r.x, b[i].r.y, b[i].r.w, b[i].r.h, b[i].label,
                     active ? col::accent : col::btnIdle, active ? col::bg : col::text,
                     b[i].r.h * 0.28f);
        }
    }

    if (kpA > 0.001f) {
        Rect kp = keypadPanelRect(W, contentH, kpA);
        // Cull text already emitted under the panel (+ its meta bar) — without
        // this, TABLE rows composite over the editor display box (text draws in
        // a later pass than panel rects, so draw order alone can't hide it).
        float top = kp.y - (st->mode != AppMode::Run ? eqMetaBarH(contentH) : 0.0f);
        c.occlude(0, top, W, contentH - top);
        drawKeypad(c, st, kp);
    }
    // Each modal occludes everything beneath it (one call → a strict clean layer;
    // no per-screen "skip drawing X" hacks). The topmost one wins.
    if (st->listOpen)      { c.occlude(0, 0, W, contentH); drawEqList(c, st, W, contentH); }
    if (st->sheetMenuOpen) { c.occlude(0, 0, W, contentH); drawSheetMenu(c, st, W, contentH); }
    if (st->renameOpen)    { c.occlude(0, 0, W, contentH); drawRename(c, st, W, contentH); }
    if (st->tableSetupOpen){ c.occlude(0, 0, W, contentH); drawTableSetup(c, st, W, contentH); }

    st->host.renderFrame(curves, quads);
}

void setupGestures(AppState* st) {
    st->gesture.onPanStart = [st](float x, float y) {
        float contentH = contentHeight(st);
        st->panAllowed = st->mode == AppMode::Graph && !st->listOpen && !st->keypadOpen &&
                         st->plotRect.contains(x, y);
        st->tabDrag = st->eqDrag = st->tableDrag = st->runDrag = false;
        if (st->listOpen && !st->sheetMenuOpen && !st->renameOpen) {
            EqListGeom g = eqListGeom(st, static_cast<float>(st->host.width()), contentH);
            if (g.tabBar.contains(x, y))        st->tabDrag = true;   // horizontal: tabs
            else if (g.rowsView.contains(x, y)) st->eqDrag = true;    // vertical: rows
        } else if (st->mode == AppMode::Table && !st->keypadOpen && !st->tableSetupOpen) {
            float topH = contentH * 0.055f, botH = toolbarHeight(contentH);
            if (y > topH && y < contentH - botH) st->tableDrag = true;
        } else if (st->mode == AppMode::Run) {  // history tape above the docked keypad
            float topH = contentH * 0.055f;
            float kpTop = keypadPanelRect(static_cast<float>(st->host.width()), contentH, 1.0f).y;
            if (y > topH && y < kpTop) st->runDrag = true;
        }
        if (st->panAllowed && st->traceMode) updateTrace(st, x);
    };
    st->gesture.onPanMove = [st](float dx, float dy, float, float) {
        if (st->tabDrag)   { st->tabScroll   -= dx; st->dirty = true; return; }
        if (st->eqDrag)    { st->eqScroll    -= dy; st->dirty = true; return; }
        if (st->tableDrag) { st->tableScroll -= dy; st->dirty = true; return; }
        if (st->runDrag)   { st->runScroll += dy; st->runScrollX -= dx; st->dirty = true; return; }
        if (!st->panAllowed) return;
        if (st->traceMode) updateTrace(st, st->touchX);
        else { st->plotview.viewport().panPixels(dx, dy); st->dirty = true; }
    };
    st->gesture.onLongPress = [st](float x, float y) {
        if (!st->listOpen || st->renameOpen || st->sheetMenuOpen) return;
        EqListGeom g = eqListGeom(st, static_cast<float>(st->host.width()), contentHeight(st));
        if (!g.tabBar.contains(x, y)) return;
        for (int i = 0; i < g.tabN; i++)
            if (g.tab[i].contains(x, y)) {
                st->menuSheetId = g.tabId[i];
                st->menuX = g.tab[i].x;
                st->menuY = g.tab[i].y + g.tab[i].h;
                st->sheetMenuOpen = true;
                st->longPressConsumed = true;  // suppress this gesture's onRelease
                st->dirty = true;
                return;
            }
    };
    st->gesture.onRelease = [st](float x, float y, bool wasPan) {
        if (st->tabDrag || st->eqDrag || st->tableDrag || st->runDrag) {
            st->tabDrag = st->eqDrag = st->tableDrag = st->runDrag = false;  // scroll finished
        }
        if (st->longPressConsumed) { st->longPressConsumed = false; return; }
        float W = static_cast<float>(st->host.width());
        float contentH = contentHeight(st);
        if (st->tableSetupOpen) { if (!wasPan) handleTableSetupTap(st, x, y); return; }
        if (st->renameOpen)    { if (!wasPan) handleRenameTap(st, x, y);    return; }
        if (st->sheetMenuOpen) { if (!wasPan) handleSheetMenuTap(st, x, y); return; }
        if (st->listOpen)   { if (!wasPan) handleEqListTap(st, x, y); return; }
        if (st->keypadOpen) {
            Rect panel = keypadPanelRect(W, contentH, st->keypadAnim);
            if (!wasPan && handleEqMetaTap(st, panel, x, y)) return;  // type/colour bar
            if (panel.contains(x, y)) handleKeypadTap(st, x, y);
            else closeEqEditor(st);                              // tap outside closes
            return;
        }
        if (st->mode == AppMode::Run) {  // docked keypad
            Rect panel = keypadPanelRect(W, contentH, 1.0f);
            // Release-based: register the key UNDER THE LIFT point even if the finger
            // slid there from another key (slide off the panel to cancel). Matches the
            // graph-mode keypad above; no more "perfect tap or nothing".
            if (panel.contains(x, y)) { handleKeypadTap(st, x, y); return; }
            if (!wasPan) {  // CAS action bar (d/dx, ∫, exact, decimal)
                Rect cbar[4]; casBarRects(W, contentH, cbar);
                for (int i = 0; i < 4; i++)
                    if (cbar[i].contains(x, y)) { casAction(st, kCasOps[i]); return; }
            }
            // taps in the tape fall through to the status strip (mode selector)
        }
        if (wasPan) return;
        float topH = contentH * 0.055f;
        if (y < topH) {  // status strip
            if (modePillRect(W, topH).contains(x, y)) { cycleMode(st); return; }  // mode selector
            if (anglePillRect(W, topH).contains(x, y)) {  // RAD ↔ DEG, all surfaces
                st->degrees = !st->degrees;
                if (st->runCalc.degrees() != st->degrees) st->runCalc.input("degrad");
                st->runPreviewDirty = true;
                st->dirty = true;
                return;
            }
            if (st->mode == AppMode::Table && tableSetupBtnRect(W, topH).contains(x, y)) {
                openTableSetup(st); return;
            }
            if (st->mode != AppMode::Run) { st->listOpen = true; st->dirty = true; }  // Run has no eqns
            return;
        }
        if (st->mode == AppMode::Graph && st->plotRect.contains(x, y)) {
            if (st->traceMode) {
                Rect chips[16]; int ids[16];              // selection chips first
                int n = traceChips(st, chips, ids);
                for (int i = 0; i < n; i++)
                    if (chips[i].contains(x, y)) {
                        auto& v = st->traceIds;
                        auto it = std::find(v.begin(), v.end(), ids[i]);
                        if (it == v.end()) v.push_back(ids[i]); else v.erase(it);
                        st->dirty = true;
                        return;
                    }
                updateTrace(st, x);
            }
            return;
        }
        if (st->mode != AppMode::Run) handleToolbarTap(st, x, y);  // Run has no toolbar
    };
}

// Capture the persistable session (equations + view window + settings).
db::WorkspaceData snapshot(const AppState* st) {
    db::WorkspaceData w;
    w.eqs = st->eqs;
    const plot::Viewport& v = st->plotview.viewport();
    w.xmin = v.xmin; w.xmax = v.xmax; w.ymin = v.ymin; w.ymax = v.ymax;
    w.degrees    = st->degrees;
    w.selectedEq = st->selectedEq;
    w.nextEqId   = st->nextEqId;
    return w;
}

void onAppCmd(android_app* app, int32_t cmd) {
    auto* st = static_cast<AppState*>(app->userData);
    switch (cmd) {
        case APP_CMD_PAUSE:
        case APP_CMD_SAVE_STATE:
            if (st->persistent) st->store.saveSheet(st->activeSheet, snapshot(st));
            break;
        case APP_CMD_INIT_WINDOW:
            if (app->window) {
                std::string cachePath = std::string(app->activity->internalDataPath) + "/font_cache.bin";
                st->host.init(app->window, app->activity->assetManager, cachePath.c_str());
                // System-UI visibility is owned by MainActivity (Java) so the nav
                // bar survives a Recents round-trip; we only need its height here.
                st->navBarPx = static_cast<float>(vce::platform::query_nav_bar_height(app));
                st->fontLoaded = loadFontAsset(app->activity->assetManager, st->font, "fonts/font.otf");
                if (!st->persistent && st->eqs.empty()) seedEquations(st);
                st->viewInit = false;
                setupGestures(st);
                st->lastT = nowSeconds();
                st->dirty = true;
                LOGI("init: ready=%d msdf=%d %ux%u navBar=%.0f", st->host.ready(),
                     st->host.msdfFont() != nullptr, st->host.width(), st->host.height(),
                     st->navBarPx);
            }
            break;
        case APP_CMD_TERM_WINDOW:
            st->host.cleanup();
            if (st->fontLoaded) { st->font.destroy(); st->fontLoaded = false; }
            break;
        case APP_CMD_WINDOW_REDRAW_NEEDED:
        case APP_CMD_CONFIG_CHANGED:
            st->dirty = true;
            break;
        case APP_CMD_GAINED_FOCUS:
            st->dirty = true;  // MainActivity re-asserts system-UI flags in Java
            break;
        default:
            break;
    }
}

int32_t onInputEvent(android_app* app, AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    auto* st = static_cast<AppState*>(app->userData);

    size_t pc     = AMotionEvent_getPointerCount(event);
    int    action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    double t      = static_cast<double>(AMotionEvent_getEventTime(event)) * 1e-9;

    // Two-finger pinch → zoom (only on the bare graph, not over an overlay).
    if (pc >= 2 && st->mode == AppMode::Graph && !st->listOpen && !st->keypadOpen) {
        float x0 = AMotionEvent_getX(event, 0), y0 = AMotionEvent_getY(event, 0);
        float x1 = AMotionEvent_getX(event, 1), y1 = AMotionEvent_getY(event, 1);
        float dist = std::hypot(x1 - x0, y1 - y0);
        float mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        if (!st->pinching) {
            st->pinching = true;
            st->pinchPrevDist = dist;
            st->pinchPrevX = mx; st->pinchPrevY = my;
            st->gesture.onTouch(GestureRecognizer::CANCEL, x0, y0, t);
        } else if (dist > 1.0f && st->pinchPrevDist > 1.0f) {
            // Two-finger gesture = pan (centroid delta) + zoom (distance ratio)
            // in one motion, like every map app.
            st->plotview.viewport().panPixels(mx - st->pinchPrevX, my - st->pinchPrevY);
            st->plotview.viewport().zoomAbout(mx, my, static_cast<double>(st->pinchPrevDist) / dist);
            st->pinchPrevDist = dist;
            st->pinchPrevX = mx; st->pinchPrevY = my;
            st->dirty = true;
        }
        return 1;
    }
    if (st->pinching) { st->pinching = false; return 1; }

    // Stylus → no haptic (protect S Pen precision); finger → haptic on keypress.
    st->lastToolStylus =
        (AMotionEvent_getToolType(event, 0) == AMOTION_EVENT_TOOL_TYPE_STYLUS);

    // DEL press-and-hold repeat, handled outside the gesture recognizer so the
    // release doesn't double-delete. Tap = one delete; hold 300ms → 50ms repeat.
    float ex = AMotionEvent_getX(event, 0), ey = AMotionEvent_getY(event, 0);
    if (action == AMOTION_EVENT_ACTION_DOWN && keypadVisible(st) &&
        delKeyRect(st).contains(ex, ey)) {
        backspaceActive(st);
        if (!st->lastToolStylus) saf::haptic(app);
        double now = nowSeconds();
        st->delActive = true; st->delDownTime = now; st->delLastRepeat = now;
        return 1;
    }
    if (st->delActive) {
        if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL)
            st->delActive = false;
        return 1;  // consume the rest of the DEL gesture
    }

    int g;
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:   g = GestureRecognizer::DOWN;   break;
        case AMOTION_EVENT_ACTION_MOVE:   g = GestureRecognizer::MOVE;   break;
        case AMOTION_EVENT_ACTION_UP:     g = GestureRecognizer::UP;     break;
        case AMOTION_EVENT_ACTION_CANCEL: g = GestureRecognizer::CANCEL; break;
        default: return 0;
    }
    st->touchX = AMotionEvent_getX(event, 0);
    st->touchY = AMotionEvent_getY(event, 0);
    st->gesture.onTouch(g, st->touchX, st->touchY, t);
    return 1;
}

}  // namespace

void android_main(android_app* state) {
    AppState st;
    st.app = state;
    state->userData     = &st;
    state->onAppCmd     = onAppCmd;
    state->onInputEvent = onInputEvent;

    // Restore the last session from SQLite before the first frame. Empty eqs ⇒
    // first launch (INIT_WINDOW seeds the demo set instead).
    if (state->activity && state->activity->internalDataPath) {
        std::string dbPath = std::string(state->activity->internalDataPath) + "/workspace.db";
        if (st.store.open(dbPath)) {
            st.persistent = true;
            int64_t active = 0;
            st.store.listSheets(st.sheets, active);
            if (st.sheets.empty()) {
                // First run: create Sheet 1, seed the demo set, persist it.
                int64_t id = st.store.createSheet("Sheet 1");
                st.activeSheet = id;
                st.store.setActiveSheet(id);
                seedEquations(&st);
                st.store.saveSheet(id, snapshot(&st));
                st.store.listSheets(st.sheets, active);
            } else {
                st.activeSheet = active ? active : st.sheets.front().id;
                db::WorkspaceData wd;
                if (st.store.loadSheet(st.activeSheet, wd)) {
                    st.eqs        = std::move(wd.eqs);
                    st.degrees    = wd.degrees;
                    st.selectedEq = st.eqs.empty() ? 0
                        : std::min(std::max(0, wd.selectedEq),
                                   static_cast<int>(st.eqs.size()) - 1);
                    st.nextEqId   = wd.nextEqId > 0 ? wd.nextEqId
                                                    : static_cast<int>(st.eqs.size()) + 1;
                    st.haveRestore = true;
                    st.rXmin = wd.xmin; st.rXmax = wd.xmax;
                    st.rYmin = wd.ymin; st.rYmax = wd.ymax;
                }
            }
        }
    }

    while (true) {
        int events;
        android_poll_source* source;
        auto needFrame = [&] { return st.host.ready() && st.dirty; };
        // Background ticks (when not already drawing): DEL repeat (~33/s), caret
        // blink (~8/s), live-preview debounce (~8/s). Otherwise sleep on events.
        auto wakeMs = [&]() -> int {
            if (st.delActive) return 30;
            if (st.casPending) return 30;   // poll the sandboxed CAS for its reply
            if (st.tableSetupOpen || st.renameOpen) return 120;
            if (st.mode == AppMode::Run && st.runPreviewDirty) return 130;
            return -1;
        };
        int timeout = needFrame() ? 0 : wakeMs();
        while (ALooper_pollOnce(timeout, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(state, source);
            if (state->destroyRequested) {
                if (st.persistent) st.store.saveSheet(st.activeSheet, snapshot(&st));
                st.host.cleanup();  // store dtor joins the writer, flushing the snapshot
                return;
            }
            timeout = needFrame() ? 0 : wakeMs();
        }

        // DEL press-and-hold: after 300ms, delete every 50ms until release.
        if (st.delActive) {
            double now = nowSeconds();
            if (now - st.delDownTime >= 0.30 && now - st.delLastRepeat >= 0.05) {
                backspaceActive(&st);
                st.delLastRepeat = now;
            }
        }
        // Keep redrawing while a background tick is pending (blink / repeat / debounce).
        if (st.host.ready() && (st.delActive || st.tableSetupOpen || st.renameOpen ||
                                st.casPending ||
                                (st.mode == AppMode::Run && st.runPreviewDirty)))
            st.dirty = true;

        // Apply a workspace that arrived from the SAF import picker (off-thread).
        std::string importedJson;
        if (st.persistent && saf::pollImport(importedJson) &&
            st.store.importJson(importedJson)) {
            int64_t active = 0;
            st.store.listSheets(st.sheets, active);
            st.activeSheet = active ? active
                           : (st.sheets.empty() ? 0 : st.sheets.front().id);
            db::WorkspaceData wd;
            if (st.store.loadSheet(st.activeSheet, wd)) applyWorkspace(&st, wd);
            st.tabScroll = 0.0f;
            st.listOpen  = true;   // surface the freshly imported sheets
            st.dirty     = true;
        }

        // Deliver a finished CAS computation (ran off-thread) onto the Run tape.
        // The render thread never touched the engine — just this poll, exactly
        // like saf::pollImport above. Stale (superseded) replies are ignored.
        {
            uint64_t tok = 0;
            cas::Reply rep;
            if (st.cas.poll(tok, rep) && tok == st.casToken) {
                // Parse the raw result ONCE into an AST for 2D natural display
                // (null AST → 1D casPretty fallback at draw time).
                TapeEntry te;
                te.label       = st.casPendingLabel;
                te.labelPrefix = st.casPendingPrefix;
                te.labelAst    = std::move(st.casPendingAst);
                te.resultRaw   = rep.ok ? rep.text : rep.error;
                if (rep.ok) {
                    std::vector<mathx::Token> toks;
                    // CAS output (Eigenmath) is space-multiplied ("2 x") → allow
                    // implicit mult HERE only, so 2D results still parse/render.
                    if (mathx::tokenize(te.resultRaw, toks, /*implicitMul=*/true)) {
                        mathx::ParseResult ppr = mathx::parse(toks);
                        if (ppr.ok && ppr.root) te.ast = std::move(ppr.root);
                    }
                }
                st.runHistory.push_back(std::move(te));
                if (st.runHistory.size() > 200) st.runHistory.erase(st.runHistory.begin());
                st.runScroll  = 0.0f;
                st.runScrollX = 0.0f;
                st.casPending = false;   // delivered → loop may sleep again
                st.dirty      = true;
            }
        }

        if (needFrame()) {
            if (state->window &&
                (st.host.width()  != static_cast<uint32_t>(ANativeWindow_getWidth(state->window)) ||
                 st.host.height() != static_cast<uint32_t>(ANativeWindow_getHeight(state->window)))) {
                st.host.cleanup();
                st.host.init(state->window, state->activity->assetManager);
                st.navBarPx = static_cast<float>(vce::platform::query_nav_bar_height(state));
                st.viewInit = false;
            }

            double t = nowSeconds();
            float dt = static_cast<float>(t - st.lastT);
            st.lastT = t;
            if (dt > 0.1f) dt = 0.1f;

            // Animate the slide-up keypad; keep frames coming while it moves.
            float target = st.keypadOpen ? 1.0f : 0.0f;
            bool animating = false;
            if (st.keypadAnim != target) {
                float step = dt / 0.18f;
                if (st.keypadAnim < target) st.keypadAnim = std::min(target, st.keypadAnim + step);
                else                        st.keypadAnim = std::max(target, st.keypadAnim - step);
                animating = true;
            }

            draw(&st);
            st.dirty = animating;
        }
    }
}
