// numeric_evaluator.hh — double-precision numeric evaluator.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#pragma once
#include "evaluator.hh"

namespace mathx {

class NumericEvaluator : public IEvaluator {
public:
    EvalResult eval(const Node& root, const EvalContext& ctx) const override;
};

}  // namespace mathx
