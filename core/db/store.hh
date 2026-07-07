// store.hh — SQLite-backed workspace persistence (pure C++; no Vulkan/Android).
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Multiple named *sheets* (workspaces), each holding an equation list + view
// window + a few settings. Sheet management (list/create/rename/delete/active)
// is synchronous — it runs on user actions, not in the render loop. Per-sheet
// content writes go through save(), which hands a snapshot to a background
// thread so the 60fps loop never blocks on disk; writes coalesce per sheet
// (latest snapshot per id wins). One shared connection, SQLITE_THREADSAFE=1, so
// the synchronous reads and the writer thread interleave safely.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "graphing/equation.hh"

struct sqlite3;  // global tag (matches sqlite3.h); avoids declaring db::sqlite3

namespace db {

// One sheet's restorable content. Defaults match a fresh sheet.
struct WorkspaceData {
    std::vector<graphing::Equation> eqs;
    double xmin = -10, xmax = 10, ymin = -6, ymax = 6;  // world view window
    bool   degrees    = false;
    int    selectedEq = 0;
    int    nextEqId    = 1;
};

struct SheetInfo { int64_t id; std::string name; };

class Store {
public:
    Store() = default;
    ~Store();
    Store(const Store&)            = delete;
    Store& operator=(const Store&) = delete;

    // Open (creating/migrating) the DB at `path`, ensure schema, start the
    // writer thread. Returns false if SQLite can't open the file.
    bool open(const std::string& path);

    // ── Sheets (synchronous; UI thread) ──────────────────────────────────────
    bool    listSheets(std::vector<SheetInfo>& out, int64_t& activeId);
    int64_t createSheet(const std::string& name);          // returns new id (0=fail)
    void    renameSheet(int64_t id, const std::string& name);
    void    deleteSheet(int64_t id);
    void    setActiveSheet(int64_t id);

    // ── Per-sheet content ────────────────────────────────────────────────────
    bool loadSheet(int64_t id, WorkspaceData& out);        // synchronous read
    void saveSheet(int64_t id, const WorkspaceData& in);   // async write (coalesced)
    void saveSheetSync(int64_t id, const WorkspaceData& in);  // blocking write (main conn)

    // ── JSON export / import of the whole workspace (synchronous) ─────────────
    std::string exportJson();                  // all sheets → pretty JSON
    bool        importJson(const std::string& json);  // replace all sheets; false=bad JSON

private:
    void workerLoop();
    void writeSheet(int64_t id, const WorkspaceData& w);    // on the writer thread

    sqlite3*        db_  = nullptr;  // main connection (UI thread: reads + sheet mgmt)
    sqlite3*        wdb_ = nullptr;  // writer connection (background thread only)
    std::thread     worker_;
    std::mutex      mu_;
    std::condition_variable cv_;
    std::map<int64_t, WorkspaceData> pending_;  // id → latest unwritten snapshot
    bool            stop_   = false;
    bool            opened_ = false;
};

}  // namespace db
