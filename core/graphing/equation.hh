// equation.hh — a plottable equation (pure data; no Vulkan/Android).
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// One row of the equation list. The type is an extension "space": Function
// (y=f(x)), Parametric (x=X(t),y=Y(t)) and Polar (r=R(t)) all sample live (see
// core/graphing/plot.cc). Parametric/Polar use the ASCII parameter `t`.
#pragma once
#include <cstdint>
#include <string>

namespace graphing {

enum class EqType { Function, Parametric, Polar };

struct RGBA { float r, g, b, a; };

struct Equation {
    int         id      = 0;
    EqType      type    = EqType::Function;
    std::string expr;            // Function: f(x); Polar: r(θ)
    std::string exprX;           // Parametric: x(t)
    std::string exprY;           // Parametric: y(t)
    RGBA        color   = {0.18f, 0.85f, 0.40f, 1.0f};
    bool        enabled = true;
    // Parametric/Polar domain (Function uses the viewport x-range). 0 step = auto.
    double      tmin = 0.0, tmax = 6.28318530717958648, tstep = 0.0;
};

}  // namespace graphing
