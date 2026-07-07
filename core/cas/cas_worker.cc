// cas_worker.cc — CasEngine sandbox thread (mirrors store.cc's writer pattern).
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "cas/cas_worker.hh"

#include <utility>

#include "cas/symbolic_engine.hh"
#ifdef CAS_USE_EIGENMATH
#include "cas/eigenmath_engine.hh"
#endif

namespace cas {

// Default backend: the real Eigenmath CAS when vendored in, else the dyno.
static std::unique_ptr<CasEngine> defaultEngine() {
#ifdef CAS_USE_EIGENMATH
    return makeEigenmathEngine();
#else
    return std::make_unique<SymbolicEngine>();
#endif
}

CasWorker::CasWorker() : CasWorker(defaultEngine()) {}

CasWorker::CasWorker(std::unique_ptr<CasEngine> engine)
    : engine_(std::move(engine)) {
    worker_ = std::thread(&CasWorker::loop, this);
}

CasWorker::~CasWorker() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cancel_.store(true);   // let any in-flight computation bail out promptly
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
}

uint64_t CasWorker::submit(const Request& req) {
    uint64_t token;
    {
        std::lock_guard<std::mutex> lk(mu_);
        token     = nextToken_++;
        req_      = req;
        reqToken_ = token;
        hasReq_   = true;
    }
    cancel_.store(true);   // supersede whatever is running; loop resets it
    cv_.notify_one();
    return token;
}

bool CasWorker::poll(uint64_t& token, Reply& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!hasReply_) return false;
    out       = std::move(reply_);
    token     = replyToken_;
    hasReply_ = false;
    return true;
}

void CasWorker::cancel() { cancel_.store(true); }

void CasWorker::loop() {
    for (;;) {
        Request  r;
        uint64_t tok;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return hasReq_ || stop_; });
            if (stop_ && !hasReq_) return;
            r       = std::move(req_);
            tok     = reqToken_;
            hasReq_ = false;
            cancel_.store(false);  // fresh request starts uncancelled
            busy_.store(true);
        }

        Reply rep = engine_->evaluate(r, cancel_);  // the only call into the CAS

        {
            std::lock_guard<std::mutex> lk(mu_);
            // Drop the result if a newer request superseded this one mid-flight;
            // the newer one will produce the reply the UI is waiting for.
            if (!hasReq_) {
                reply_      = std::move(rep);
                replyToken_ = tok;
                hasReply_   = true;
            }
            busy_.store(false);
        }
    }
}

}  // namespace cas
