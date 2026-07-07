// eigenmath_engine.hh — the next backend behind the SAME CasEngine seam.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// This is the documented drop-in point for the real CAS. The dyno (SymbolicEngine)
// proves the seam + worker isolation + UI; swapping in Eigenmath is mechanical
// because nothing above CasEngine knows which backend is bolted in:
//
//   CasWorker worker(cas::makeEigenmathEngine());   // instead of the default
//
// VENDORING (mirrors how core/db/sqlite3.c was vendored — a dependency-free C/C++
// blob dropped straight into the build):
//   1. Place Eigenmath's amalgamated source under  core/cas/eigenmath/
//      (eigenmath.cpp + defs.h). It has NO external deps — cleanest NDK path of
//      the candidates we evaluated (BSD-style licence; fits our AGPLv3 umbrella).
//   2. In app/src/main/CMakeLists.txt: add  ${CORE_DIR}/cas/eigenmath/eigenmath.cpp
//      and  ${CORE_DIR}/cas/eigenmath_engine.cc  to the calculator target, and add
//          target_compile_definitions(calculator PRIVATE CAS_USE_EIGENMATH)
//   3. eigenmath_engine.cc (compiled only under CAS_USE_EIGENMATH) wraps the
//      Eigenmath interpreter: feed `req.expr` (rewriting d/dx → d(expr,var),
//      ∫ → integral(expr,var)), run one evaluation, read the result back, and
//      poll `cancel` via Eigenmath's interrupt hook so a hard integral can be
//      abandoned without touching the render thread.
//
// When Giac becomes the engine later, it slots in the exact same way
// (makeGiacEngine()), and this header's contract does not change.
#pragma once
#include <memory>

#include "cas/cas.hh"

namespace cas {

// Defined in eigenmath_engine.cc, compiled only when CAS_USE_EIGENMATH is set
// and the vendored source is present. Until then the default SymbolicEngine runs.
std::unique_ptr<CasEngine> makeEigenmathEngine();

}  // namespace cas
