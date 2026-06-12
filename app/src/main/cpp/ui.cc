#include "ui.hh"
#include "glyphs.hh"
#include <android/log.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int   MAX_DISPLAY_CHARS = 12;
constexpr float DISPLAY_HEIGHT_FRAC = 0.20f;   // display takes top 20% of screen
constexpr float BUTTON_PADDING_FRAC = 0.012f;  // gap between buttons (frac of screenW)

// Colors (linear, premultiplication is done by the shader path).
constexpr float BG_R = 0.07f, BG_G = 0.07f, BG_B = 0.10f;
constexpr float DISPLAY_R = 0.04f, DISPLAY_G = 0.04f, DISPLAY_B = 0.06f;
constexpr float BTN_R = 0.18f, BTN_G = 0.18f, BTN_B = 0.22f;
constexpr float BTN_OP_R = 0.30f, BTN_OP_G = 0.20f, BTN_OP_B = 0.10f;   // operators tinted
constexpr float BTN_EQ_R = 0.10f, BTN_EQ_G = 0.30f, BTN_EQ_B = 0.20f;   // equals tinted
constexpr float TEXT_R = 1.0f, TEXT_G = 1.0f, TEXT_B = 1.0f;

bool isOperatorAction(Action a) {
  return a == Action::ADD || a == Action::SUB ||
         a == Action::MUL || a == Action::DIV;
}

} // namespace

void Calculator::init(uint32_t w, uint32_t h) {
  __android_log_print(ANDROID_LOG_DEBUG, "DBG", "Calculator::init w=%u h=%u", w, h);
  screenW = w;
  screenH = h;

  const float pad        = BUTTON_PADDING_FRAC * float(w);
  const float displayH   = DISPLAY_HEIGHT_FRAC * float(h);
  const float gridY0     = displayH;
  const float gridH      = float(h) - displayH;
  const float cellW      = float(w) / 4.0f;
  const float cellH      = gridH    / 5.0f;

  struct Spec { Action a; const char* label; int digit; };
  static const Spec layout[5][4] = {
    {{Action::CLEAR,"C",0},  {Action::PERCENT,"%",0}, {Action::SQRT,"r",0},  {Action::DIV,"/",0}},
    {{Action::DIGIT,"7",7},  {Action::DIGIT,"8",8},   {Action::DIGIT,"9",9}, {Action::MUL,"x",0}},
    {{Action::DIGIT,"4",4},  {Action::DIGIT,"5",5},   {Action::DIGIT,"6",6}, {Action::SUB,"-",0}},
    {{Action::DIGIT,"1",1},  {Action::DIGIT,"2",2},   {Action::DIGIT,"3",3}, {Action::ADD,"+",0}},
    {{Action::ANS,"ANS",0},  {Action::DIGIT,"0",0},   {Action::DOT,".",0},   {Action::EQ,"=",0}},
  };

  buttons.clear();
  buttons.reserve(20);
  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 4; col++) {
      const Spec& s = layout[row][col];
      Button b;
      b.x      = float(col) * cellW + pad;
      b.y      = gridY0 + float(row) * cellH + pad;
      b.w      = cellW - 2.0f * pad;
      b.h      = cellH - 2.0f * pad;
      b.action = s.a;
      b.digit  = s.digit;
      b.label  = s.label;
      buttons.push_back(b);
    }
  }
}

void Calculator::onTouch(float px, float py) {
  for (const Button& b : buttons) {
    if (px >= b.x && px <= b.x + b.w &&
        py >= b.y && py <= b.y + b.h) {
      press(b);
      dirty = true;
      return;
    }
  }
}

void Calculator::press(const Button& b) {
  if (error && b.action != Action::CLEAR) {
    // Any press other than CLEAR clears the error and starts fresh.
    error      = false;
    display    = "0";
    accumulator = 0.0;
    pending    = Action::EQ;
    startedNew = true;
    if (b.action == Action::DIGIT || b.action == Action::DOT) {
      // fall through and process the press normally below
    } else {
      return;
    }
  }

  switch (b.action) {
    case Action::DIGIT: {
      char ch = char('0' + b.digit);
      if (startedNew) {
        display.assign(1, ch);
        startedNew = false;
      } else if ((int)display.size() < MAX_DISPLAY_CHARS) {
        if (display == "0") display.assign(1, ch);
        else                display.push_back(ch);
      }
      break;
    }
    case Action::DOT: {
      if (startedNew) {
        display    = "0.";
        startedNew = false;
      } else if (display.find('.') == std::string::npos &&
                 (int)display.size() < MAX_DISPLAY_CHARS) {
        display.push_back('.');
      }
      break;
    }
    case Action::ADD: case Action::SUB:
    case Action::MUL: case Action::DIV: {
      applyPending();
      pending    = b.action;
      startedNew = true;
      break;
    }
    case Action::EQ: {
      applyPending();
      lastAns    = accumulator;
      pending    = Action::EQ;
      startedNew = true;
      break;
    }
    case Action::CLEAR: {
      display     = "0";
      accumulator = 0.0;
      pending     = Action::EQ;
      startedNew  = true;
      error       = false;
      break;
    }
    case Action::SQRT: {
      double v = parseDisplay();
      if (v < 0.0) {
        display = "Err";
        error   = true;
      } else {
        setDisplayFromValue(std::sqrt(v));
      }
      startedNew = true;
      break;
    }
    case Action::PERCENT: {
      double v = parseDisplay();
      setDisplayFromValue(v / 100.0);
      startedNew = true;
      break;
    }
    case Action::ANS: {
      setDisplayFromValue(lastAns);
      startedNew = true;
      break;
    }
  }
}

void Calculator::applyPending() {
  double rhs = parseDisplay();
  double result = accumulator;
  switch (pending) {
    case Action::ADD: result = accumulator + rhs; break;
    case Action::SUB: result = accumulator - rhs; break;
    case Action::MUL: result = accumulator * rhs; break;
    case Action::DIV:
      if (rhs == 0.0) { display = "Err"; error = true; accumulator = 0.0; return; }
      result = accumulator / rhs; break;
    case Action::EQ: default: result = rhs; break;
  }
  accumulator = result;
  setDisplayFromValue(result);
}

double Calculator::parseDisplay() const {
  if (display.empty()) return 0.0;
  return std::strtod(display.c_str(), nullptr);
}

void Calculator::setDisplayFromValue(double v) {
  if (std::isnan(v) || std::isinf(v)) {
    display = "Err";
    error   = true;
    return;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  display = buf;
  if ((int)display.size() > MAX_DISPLAY_CHARS) {
    // Number doesn't fit; fall back to compact scientific.
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    display = buf;
    if ((int)display.size() > MAX_DISPLAY_CHARS) {
      display.resize(MAX_DISPLAY_CHARS);
    }
  }
}

void Calculator::rebuildCurves(std::vector<float>& out) const {
  out.clear();
  out.reserve(400 * 16);
  __android_log_print(ANDROID_LOG_DEBUG, "DBG",
      "rebuildCurves: screenW=%u screenH=%u buttons=%zu display=\"%s\"",
      screenW, screenH, buttons.size(), display.c_str());

  // Background fill (full screen).
  emitFilledRect(out,
                 float(screenW) * 0.5f, float(screenH) * 0.5f,
                 float(screenW) * 0.5f, float(screenH) * 0.5f,
                 BG_R, BG_G, BG_B, 1.0f);

  // Display panel background.
  const float pad        = BUTTON_PADDING_FRAC * float(screenW);
  const float displayH   = DISPLAY_HEIGHT_FRAC * float(screenH);
  emitFilledRect(out,
                 float(screenW) * 0.5f, displayH * 0.5f,
                 float(screenW) * 0.5f - pad, displayH * 0.5f - pad,
                 DISPLAY_R, DISPLAY_G, DISPLAY_B, 1.0f);

  // Display text, right-aligned within the display panel.
  {
    const float textHeight = displayH * 0.55f;
    const float scale      = textHeight;
    float total = 0.0f;
    for (char c : display) total += glyphAdvance(c, scale);
    const float rightEdge = float(screenW) - pad * 2.0f;
    const float baselineY = displayH * 0.5f - textHeight * 0.5f;
    float x = rightEdge - total;
    for (char c : display) {
      emitGlyph(out, c, x, baselineY, scale, 2.0f,
                TEXT_R, TEXT_G, TEXT_B, 1.0f);
      x += glyphAdvance(c, scale);
    }
  }

  // Buttons.
  for (const Button& b : buttons) {
    float r = BTN_R, g = BTN_G, bb = BTN_B;
    if (isOperatorAction(b.action)) { r = BTN_OP_R; g = BTN_OP_G; bb = BTN_OP_B; }
    else if (b.action == Action::EQ) { r = BTN_EQ_R; g = BTN_EQ_G; bb = BTN_EQ_B; }

    emitFilledRect(out,
                   b.x + b.w * 0.5f, b.y + b.h * 0.5f,
                   b.w * 0.5f, b.h * 0.5f,
                   r, g, bb, 1.0f);

    // Label centered in button. For multi-char labels, scale shrinks to fit.
    const float maxTextHeight = b.h * 0.55f;
    const float maxTextWidth  = b.w * 0.75f;
    float scale = maxTextHeight;
    float total = 0.0f;
    for (const char* p = b.label; *p; p++) total += glyphAdvance(*p, scale);
    if (total > maxTextWidth) {
      scale *= maxTextWidth / total;
      total  = 0.0f;
      for (const char* p = b.label; *p; p++) total += glyphAdvance(*p, scale);
    }
    float x = b.x + (b.w - total) * 0.5f;
    float y = b.y + (b.h - scale) * 0.5f;
    for (const char* p = b.label; *p; p++) {
      emitGlyph(out, *p, x, y, scale, 2.0f,
                TEXT_R, TEXT_G, TEXT_B, 1.0f);
      x += glyphAdvance(*p, scale);
    }
  }
}
