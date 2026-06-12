#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class Action {
  DIGIT, DOT, ADD, SUB, MUL, DIV, EQ, CLEAR, SQRT, PERCENT, ANS
};

struct Button {
  float       x, y, w, h;   // pixel rect (top-left + size)
  Action      action;
  int         digit;        // for DIGIT only
  const char* label;        // string of glyph chars to render
};

class Calculator {
 public:
  void init(uint32_t screenW, uint32_t screenH);
  void onTouch(float px, float py);
  void rebuildCurves(std::vector<float>& out) const;

  bool dirty = true;        // true on first frame so we draw immediately

 private:
  void press(const Button& b);
  void applyPending();
  void setDisplayFromValue(double v);
  double parseDisplay() const;

  uint32_t screenW = 0;
  uint32_t screenH = 0;
  std::vector<Button> buttons;

  std::string display = "0";
  double accumulator  = 0.0;
  Action pending      = Action::EQ;
  double lastAns      = 0.0;
  bool   startedNew   = true;
  bool   error        = false;
};
