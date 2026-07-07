// eigenmath_engine.cc — CasEngine backed by the vendored Eigenmath CAS.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
// (Eigenmath itself is BSD-2-Clause; see core/cas/eigenmath/LICENSE.)
//
// Compiled only when CAS_USE_EIGENMATH is defined and core/cas/eigenmath/eigenmath.c
// is in the build. This is the McLaren's first real engine: it brings symbolic
// integration and true exact arithmetic (fractions, surds, π) that the homegrown
// dyno can't. It slots behind the unchanged cas::CasEngine seam.
//
// Bridging Eigenmath (a single-global-state C interpreter) into our sandbox:
//   * INPUT: our canonical ASCII is parsed by the production lexer/parser (so it
//     is validated and implicit multiplication is made explicit), then serialized
//     into Eigenmath's dialect (ln→log, base-10 log→log/log(10), e→exp(1)).
//   * OUTPUT: `tty=1` switches Eigenmath to one-line "string format"; printbuf is
//     captured via the vendored output hook into a std::string.
//   * CANCEL: Eigenmath polls a global `interrupt` in its eval loop. A watcher
//     thread raises it when our cooperative `cancel` flips, so a runaway integral
//     unwinds (longjmp) back out of run() — the render thread is never touched.
//   * SERIALIZED: one global interpreter ⇒ exactly one eval at a time. CasWorker
//     already guarantees that (one engine, one thread).
#ifdef CAS_USE_EIGENMATH

#include "cas/eigenmath_engine.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "math/ast.hh"
#include "math/lexer.hh"
#include "math/parser.hh"

// ── Vendored Eigenmath C interface (core/cas/eigenmath/eigenmath.c) ──────────
extern "C" {
void run(char* buf);                              // evaluate one input buffer
extern int interrupt;                             // set !=0 to stop the eval loop
extern void (*eigenmath_output_hook)(const char*);// printbuf capture (vendored edit)
}

namespace cas {
namespace {

using mathx::Node;

// Output capture: Eigenmath runs on one thread, so a file-scope sink is safe.
std::string* g_capture = nullptr;
extern "C" void eigenmath_capture(const char* s) {
    if (g_capture && s) g_capture->append(s);
}

// ── Serialize our AST into Eigenmath's input dialect (explicit operators) ────
std::string fmtNum(double v) {
    if (v == 0.0) return "0";
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}
int prec(const Node& n) {
    switch (n.kind) {
        case Node::Add: case Node::Sub: return 1;
        case Node::Mul: case Node::Div: return 2;
        case Node::Neg:                 return 2;
        case Node::Pow:                 return 3;
        default:                        return 4;
    }
}
void emit(const Node& n, std::string& o, int ctx) {
    bool numNeg = (n.kind == Node::Num && n.num < 0);
    bool paren  = prec(n) < ctx || (numNeg && ctx >= 2);
    if (paren) o += '(';
    switch (n.kind) {
        case Node::Num: o += fmtNum(n.num); break;
        case Node::Const:
            o += (n.name == "e") ? "exp(1)" : n.name;   // π is "pi"; e → exp(1)
            break;
        case Node::Var:  o += n.name; break;
        case Node::Neg:  o += '-'; emit(*n.kids[0], o, 2); break;
        case Node::Add:  emit(*n.kids[0], o, 1); o += '+'; emit(*n.kids[1], o, 1); break;
        case Node::Sub:  emit(*n.kids[0], o, 1); o += '-'; emit(*n.kids[1], o, 2); break;
        case Node::Mul:  emit(*n.kids[0], o, 2); o += '*'; emit(*n.kids[1], o, 2); break;
        case Node::Div:  emit(*n.kids[0], o, 2); o += '/'; emit(*n.kids[1], o, 3); break;
        case Node::Pow:  emit(*n.kids[0], o, 4); o += '^'; emit(*n.kids[1], o, 3); break;
        case Node::Call:
            if (n.name == "ln") {                       // our ln = natural log = Eigenmath log
                o += "log("; emit(*n.kids[0], o, 0); o += ')';
            } else if (n.name == "log") {               // our log = base 10
                o += "(log("; emit(*n.kids[0], o, 0); o += ")/log(10))";
            } else {                                    // sin cos tan sqrt
                o += n.name; o += '('; emit(*n.kids[0], o, 0); o += ')';
            }
            break;
    }
    if (paren) o += ')';
}

std::string toEigenmath(const std::string& src, std::string& err) {
    std::vector<mathx::Token> toks;
    if (!mathx::tokenize(src, toks)) { err = "Syntax Error"; return ""; }
    mathx::ParseResult pr = mathx::parse(toks);
    if (!pr.ok || !pr.root)          { err = "Syntax Error"; return ""; }
    std::string o;
    emit(*pr.root, o, 0);
    return o;
}

std::string buildCmd(Op op, const std::string& e, const std::string& var) {
    switch (op) {
        case Op::Derivative: return "d(" + e + "," + var + ")";
        case Op::Integral:   return "integral(" + e + "," + var + ")";
        case Op::Numeric:    return "float(" + e + ")";
        case Op::Simplify:   return "simplify(" + e + ")";
    }
    return e;
}

// Collapse Eigenmath's output to a single trimmed tape line.
std::string collapse(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    size_t a = o.find_first_not_of(' ');
    size_t b = o.find_last_not_of(' ');
    if (a == std::string::npos) return "";
    o = o.substr(a, b - a + 1);
    std::string r;                       // squeeze runs of spaces
    for (char c : o) { if (c == ' ' && !r.empty() && r.back() == ' ') continue; r += c; }
    return r;
}

class EigenmathEngine : public CasEngine {
public:
    Reply evaluate(const Request& req, const std::atomic<bool>& cancel) override {
        Reply rep;
        std::string err;
        std::string expr = toEigenmath(req.expr, err);
        if (expr.empty()) { rep.error = err.empty() ? "Syntax Error" : err; return rep; }

        ensureInit();
        if (cancel.load()) { rep.error = "Interrupted"; return rep; }

        std::string cmd = buildCmd(req.op, expr, req.var.empty() ? "x" : req.var);
        std::string out = runCapture(cmd, &cancel);

        if (cancel.load() || out.find("Stop: interrupt") != std::string::npos) {
            rep.error = "Interrupted"; return rep;
        }
        size_t stop = out.find("Stop: ");
        if (stop != std::string::npos) { rep.error = collapse(out.substr(stop + 6)); return rep; }
        std::string text = collapse(out);
        if (text.empty()) { rep.error = "(no result)"; return rep; }
        rep.ok = true; rep.text = text;
        return rep;
    }

    const char* name() const override { return "eigenmath"; }

private:
    bool inited_ = false;

    // One-time: trigger Eigenmath's lazy init and switch to one-line output.
    void ensureInit() {
        if (inited_) return;
        inited_ = true;
        runCapture("tty=1", nullptr);   // discard the "tty = 1" echo
    }

    // Run `cmd` through Eigenmath, capturing printbuf. While it runs, a watcher
    // raises the global `interrupt` if `cancel` flips (cooperative cancellation).
    std::string runCapture(const std::string& cmd, const std::atomic<bool>* cancel) {
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');                       // run() may scan in place

        std::string out;
        g_capture = &out;
        eigenmath_output_hook = &eigenmath_capture;
        interrupt = 0;

        std::atomic<bool> done{false};
        std::thread watcher;
        if (cancel) {
            watcher = std::thread([cancel, &done] {
                while (!done.load(std::memory_order_acquire)) {
                    if (cancel->load()) { interrupt = 1; return; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            });
        }

        run(buf.data());                           // the only call into Eigenmath

        done.store(true, std::memory_order_release);
        if (watcher.joinable()) watcher.join();
        eigenmath_output_hook = nullptr;
        g_capture = nullptr;
        return out;
    }
};

}  // namespace

std::unique_ptr<CasEngine> makeEigenmathEngine() {
    return std::make_unique<EigenmathEngine>();
}

}  // namespace cas

#endif  // CAS_USE_EIGENMATH
