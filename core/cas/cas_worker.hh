// cas_worker.hh — the sandbox: runs a CasEngine on its own thread.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// This is the wall that keeps the Vulkan 60fps loop sacred. The CAS engine (with
// all its global state, allocations, and — once we swap in Giac — its setjmp/
// signal machinery) lives ONLY on this worker thread. The render thread never
// calls evaluate(); it submit()s a request and poll()s for the reply each frame,
// exactly like saf::pollImport() and the SQLite background writer in store.cc.
//
// Cancellation is cooperative: submit() supersedes any in-flight computation by
// raising the cancel flag, and cancel() / a timeout can abandon a runaway one.
// The thread is never killed mid-flight — the engine returns "Interrupted".
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "cas/cas.hh"

namespace cas {

class CasWorker {
public:
    // Default backend is the homegrown SymbolicEngine "dyno". Pass a different
    // CasEngine (Eigenmath, later Giac) to swap engines behind the same seam.
    CasWorker();
    explicit CasWorker(std::unique_ptr<CasEngine> engine);
    ~CasWorker();
    CasWorker(const CasWorker&)            = delete;
    CasWorker& operator=(const CasWorker&) = delete;

    // Queue `req`. Returns a token identifying this submission; a later poll()
    // hands back the same token so the UI can ignore stale (superseded) replies.
    // A new submit() raises the cancel flag on whatever is in flight.
    uint64_t submit(const Request& req);

    // Non-blocking. If a reply has arrived since the last poll, move it into
    // `out`, set `token` to the submission it answers, and return true.
    bool poll(uint64_t& token, Reply& out);

    // Ask the in-flight computation to stop as soon as it can (cooperative).
    void cancel();

    bool busy() const { return busy_.load(); }
    const char* engineName() const { return engine_->name(); }

private:
    void loop();

    std::unique_ptr<CasEngine> engine_;
    std::thread                worker_;

    std::mutex              mu_;
    std::condition_variable cv_;
    Request   req_;
    Reply     reply_;
    uint64_t  reqToken_   = 0;
    uint64_t  replyToken_ = 0;
    uint64_t  nextToken_  = 1;
    bool      hasReq_     = false;
    bool      hasReply_   = false;
    bool      stop_       = false;

    std::atomic<bool> cancel_{false};  // read by the engine between sub-steps
    std::atomic<bool> busy_{false};
};

}  // namespace cas
