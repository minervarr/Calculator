// cas.hh — backend-agnostic CAS (computer-algebra) engine seam.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// This is the second pluggable seam in the math stack. The first — IEvaluator —
// turns an AST into a *number* and powers the 60fps grapher (NumericEvaluator).
// This one turns an expression into another *expression* (symbolic): simplify,
// differentiate, integrate, S<->D. It is deliberately string-in / string-out so
// any backend can sit behind it — the homegrown SymbolicEngine "dyno" today, and
// Eigenmath, then Giac, tomorrow — without the UI knowing which is bolted in.
//
// Hard rules (so the Vulkan loop stays sacred):
//   * A CasEngine runs ONLY on the CAS worker thread (see cas_worker.hh), never
//     on the render thread. No Vulkan/Android types cross this header.
//   * evaluate() MUST poll `cancel` between sub-steps. A runaway computation
//     (a hard integral) is interrupted cooperatively — the render thread flips
//     the flag, the engine bails out with an "Interrupted" reply. We never kill
//     the thread or longjmp across the boundary.
#pragma once
#include <atomic>
#include <string>

namespace cas {

// What the user asked the CAS to do with the current expression.
enum class Op {
    Simplify,    // canonical / reduced form              (also S<->D "S" path)
    Numeric,     // force an IEEE-754 decimal value        (also S<->D "D" path)
    Derivative,  // d/d<var> of the expression
    Integral,    // ∫ expr d<var>   (dyno backend stubs this; Giac fills it in)
};

struct Request {
    Op          op      = Op::Simplify;
    std::string expr;             // canonical ASCII (Editor::linearize() output)
    std::string var     = "x";    // differentiation / integration variable
    bool        degrees = false;  // angle mode, passed through to the backend
};

struct Reply {
    bool        ok = false;
    std::string text;   // canonical-ASCII result; the UI prettifies for display
    std::string error;  // when !ok: "Syntax Error" / "Unsupported" / "Interrupted" / …
};

// A pluggable symbolic backend. One instance is owned by one CasWorker and only
// ever touched from that worker's thread.
class CasEngine {
public:
    virtual ~CasEngine() = default;

    // Compute `req`. `cancel` is flipped to true (from another thread) to ask the
    // engine to abandon the in-flight computation as soon as it can.
    virtual Reply evaluate(const Request& req, const std::atomic<bool>& cancel) = 0;

    // Short identifier for logs / an "engine: …" status line.
    virtual const char* name() const = 0;
};

}  // namespace cas
