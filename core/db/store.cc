// store.cc — SQLite multi-sheet persistence with a background writer thread.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#include "db/store.hh"

#include <cstdio>
#include <cstdlib>

#include "db/json.hh"
#include "db/sqlite3.h"

namespace db {
namespace {

constexpr int kSchemaVersion = 2;  // bump → wipe-and-recreate on open (dev data)

const char* kSchema =
    "CREATE TABLE IF NOT EXISTS app("
    "  key TEXT PRIMARY KEY, value TEXT);"
    "CREATE TABLE IF NOT EXISTS sheets("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT, ord INTEGER,"
    "  xmin REAL, xmax REAL, ymin REAL, ymax REAL,"
    "  degrees INTEGER, selected INTEGER, next_eqid INTEGER);"
    "CREATE TABLE IF NOT EXISTS equations("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  sheet_id INTEGER, slot INTEGER,"
    "  eqid INTEGER, type INTEGER,"
    "  expr TEXT, exprx TEXT, expry TEXT,"
    "  cr REAL, cg REAL, cb REAL, ca REAL,"
    "  enabled INTEGER, tmin REAL, tmax REAL, tstep REAL);"
    "CREATE INDEX IF NOT EXISTS eq_sheet ON equations(sheet_id, slot);";

void exec(sqlite3* db, const char* sql) {
    sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
}

void setApp(sqlite3* db, const char* key, const std::string& val) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO app(key,value) VALUES(?,?)"
            " ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &s, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, val.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

bool getApp(sqlite3* db, const char* key, std::string& out) {
    sqlite3_stmt* s = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db, "SELECT value FROM app WHERE key=?;",
                           -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(s, 0);
            if (t) { out = reinterpret_cast<const char*>(t); found = true; }
        }
        sqlite3_finalize(s);
    }
    return found;
}

std::string d2s(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

int schemaVersion(sqlite3* db) {
    int v = 0;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

}  // namespace

Store::~Store() {
    if (opened_) {
        { std::lock_guard<std::mutex> lk(mu_); stop_ = true; }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();  // drains pending snapshots first
    }
    if (wdb_) sqlite3_close(wdb_);
    if (db_)  sqlite3_close(db_);
}

bool Store::open(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    exec(db_, "PRAGMA journal_mode=WAL;");
    exec(db_, "PRAGMA synchronous=NORMAL;");

    // Migration: the v1 build used a single-workspace schema (settings + a
    // sheet-less equations table). On any older/absent version, wipe and
    // recreate — the only data is demo graphs, which reseed on first launch.
    if (schemaVersion(db_) < kSchemaVersion) {
        exec(db_, "DROP TABLE IF EXISTS settings;");
        exec(db_, "DROP TABLE IF EXISTS equations;");
        exec(db_, "DROP TABLE IF EXISTS sheets;");
        exec(db_, "DROP TABLE IF EXISTS app;");
    }
    if (sqlite3_exec(db_, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close(db_); db_ = nullptr;
        return false;
    }
    exec(db_, "PRAGMA user_version=2;");
    exec(db_, "PRAGMA busy_timeout=4000;");

    // Separate writer connection so the background writer's transactions never
    // nest with a main-thread BEGIN (import/export). WAL lets them coexist; the
    // busy_timeout makes a contended write wait instead of erroring.
    if (sqlite3_open(path.c_str(), &wdb_) != SQLITE_OK) {
        if (wdb_) { sqlite3_close(wdb_); wdb_ = nullptr; }
        sqlite3_close(db_); db_ = nullptr;
        return false;
    }
    exec(wdb_, "PRAGMA busy_timeout=4000;");
    exec(wdb_, "PRAGMA synchronous=NORMAL;");

    opened_ = true;
    worker_ = std::thread(&Store::workerLoop, this);
    return true;
}

bool Store::listSheets(std::vector<SheetInfo>& out, int64_t& activeId) {
    if (!opened_) return false;
    out.clear();
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,name FROM sheets ORDER BY ord,id;",
                           -1, &s, nullptr) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            SheetInfo si;
            si.id = sqlite3_column_int64(s, 0);
            const unsigned char* t = sqlite3_column_text(s, 1);
            si.name = t ? reinterpret_cast<const char*>(t) : "";
            out.push_back(std::move(si));
        }
        sqlite3_finalize(s);
    }
    activeId = 0;
    std::string v;
    if (getApp(db_, "active_sheet", v)) activeId = std::atoll(v.c_str());
    return true;
}

int64_t Store::createSheet(const std::string& name) {
    if (!opened_) return 0;
    int ord = 0;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(ord),-1)+1 FROM sheets;",
                           -1, &s, nullptr) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) ord = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO sheets(name,ord,xmin,xmax,ymin,ymax,degrees,selected,next_eqid)"
            " VALUES(?,?,-10,10,-6,6,0,0,1);",
            -1, &s, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(s, 2, ord);
    int64_t id = 0;
    if (sqlite3_step(s) == SQLITE_DONE) id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(s);
    return id;
}

void Store::renameSheet(int64_t id, const std::string& name) {
    if (!opened_) return;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE sheets SET name=? WHERE id=?;",
                           -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(s, 2, id);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
}

void Store::deleteSheet(int64_t id) {
    if (!opened_) return;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM equations WHERE sheet_id=?;",
                           -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id); sqlite3_step(s); sqlite3_finalize(s);
    }
    if (sqlite3_prepare_v2(db_, "DELETE FROM sheets WHERE id=?;",
                           -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id); sqlite3_step(s); sqlite3_finalize(s);
    }
    { std::lock_guard<std::mutex> lk(mu_); pending_.erase(id); }  // cancel stale write
}

void Store::setActiveSheet(int64_t id) {
    if (opened_) setApp(db_, "active_sheet", std::to_string(id));
}

bool Store::loadSheet(int64_t id, WorkspaceData& out) {
    if (!opened_) return false;
    sqlite3_stmt* s = nullptr;
    bool found = false;
    if (sqlite3_prepare_v2(db_,
            "SELECT xmin,xmax,ymin,ymax,degrees,selected,next_eqid FROM sheets WHERE id=?;",
            -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            out.xmin = sqlite3_column_double(s, 0);
            out.xmax = sqlite3_column_double(s, 1);
            out.ymin = sqlite3_column_double(s, 2);
            out.ymax = sqlite3_column_double(s, 3);
            out.degrees    = sqlite3_column_int(s, 4) != 0;
            out.selectedEq = sqlite3_column_int(s, 5);
            out.nextEqId   = sqlite3_column_int(s, 6);
            found = true;
        }
        sqlite3_finalize(s);
    }
    if (!found) return false;

    if (sqlite3_prepare_v2(db_,
            "SELECT eqid,type,expr,exprx,expry,cr,cg,cb,ca,enabled,tmin,tmax,tstep"
            " FROM equations WHERE sheet_id=? ORDER BY slot;",
            -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        auto txt = [&](int c) {
            const unsigned char* t = sqlite3_column_text(s, c);
            return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
        };
        while (sqlite3_step(s) == SQLITE_ROW) {
            graphing::Equation e;
            e.id    = sqlite3_column_int(s, 0);
            e.type  = static_cast<graphing::EqType>(sqlite3_column_int(s, 1));
            e.expr  = txt(2);
            e.exprX = txt(3);
            e.exprY = txt(4);
            e.color = {static_cast<float>(sqlite3_column_double(s, 5)),
                       static_cast<float>(sqlite3_column_double(s, 6)),
                       static_cast<float>(sqlite3_column_double(s, 7)),
                       static_cast<float>(sqlite3_column_double(s, 8))};
            e.enabled = sqlite3_column_int(s, 9) != 0;
            e.tmin  = sqlite3_column_double(s, 10);
            e.tmax  = sqlite3_column_double(s, 11);
            e.tstep = sqlite3_column_double(s, 12);
            out.eqs.push_back(std::move(e));
        }
        sqlite3_finalize(s);
    }
    return true;
}

void Store::saveSheet(int64_t id, const WorkspaceData& in) {
    if (!opened_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_[id] = in;  // coalesce: latest snapshot per sheet wins
    }
    cv_.notify_one();
}

void Store::workerLoop() {
    for (;;) {
        std::map<int64_t, WorkspaceData> batch;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return !pending_.empty() || stop_; });
            if (!pending_.empty()) batch.swap(pending_);
            else if (stop_)        return;
        }
        for (auto& kv : batch) writeSheet(kv.first, kv.second);  // outside the lock
    }
}

// Shared by the background writer (wdb_) and the synchronous path (db_). Each
// connection has its own transaction, so the two never nest a BEGIN.
static void writeSheetInto(sqlite3* db, int64_t id, const db::WorkspaceData& w) {
    exec(db, "BEGIN IMMEDIATE;");

    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db,
            "UPDATE sheets SET xmin=?,xmax=?,ymin=?,ymax=?,degrees=?,selected=?,next_eqid=?"
            " WHERE id=?;",
            -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_double(s, 1, w.xmin);
        sqlite3_bind_double(s, 2, w.xmax);
        sqlite3_bind_double(s, 3, w.ymin);
        sqlite3_bind_double(s, 4, w.ymax);
        sqlite3_bind_int   (s, 5, w.degrees ? 1 : 0);
        sqlite3_bind_int   (s, 6, w.selectedEq);
        sqlite3_bind_int   (s, 7, w.nextEqId);
        sqlite3_bind_int64 (s, 8, id);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    if (sqlite3_prepare_v2(db, "DELETE FROM equations WHERE sheet_id=?;",
                           -1, &s, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id); sqlite3_step(s); sqlite3_finalize(s);
    }

    if (sqlite3_prepare_v2(db,
            "INSERT INTO equations(sheet_id,slot,eqid,type,expr,exprx,expry,"
            "cr,cg,cb,ca,enabled,tmin,tmax,tstep)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
            -1, &s, nullptr) == SQLITE_OK) {
        int slot = 0;
        for (const auto& e : w.eqs) {
            sqlite3_bind_int64 (s, 1,  id);
            sqlite3_bind_int   (s, 2,  slot++);
            sqlite3_bind_int   (s, 3,  e.id);
            sqlite3_bind_int   (s, 4,  static_cast<int>(e.type));
            sqlite3_bind_text  (s, 5,  e.expr.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (s, 6,  e.exprX.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (s, 7,  e.exprY.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(s, 8,  e.color.r);
            sqlite3_bind_double(s, 9,  e.color.g);
            sqlite3_bind_double(s, 10, e.color.b);
            sqlite3_bind_double(s, 11, e.color.a);
            sqlite3_bind_int   (s, 12, e.enabled ? 1 : 0);
            sqlite3_bind_double(s, 13, e.tmin);
            sqlite3_bind_double(s, 14, e.tmax);
            sqlite3_bind_double(s, 15, e.tstep);
            sqlite3_step(s);
            sqlite3_reset(s);
        }
        sqlite3_finalize(s);
    }

    exec(db, "COMMIT;");
}

void Store::writeSheet(int64_t id, const WorkspaceData& w) { writeSheetInto(wdb_, id, w); }

void Store::saveSheetSync(int64_t id, const WorkspaceData& in) {
    if (opened_) writeSheetInto(db_, id, in);
}

std::string Store::exportJson() {
    std::string o = "{\n  \"format\": \"calc-workspace\",\n  \"version\": 1,\n  \"sheets\": [";
    if (opened_) {
        sqlite3_stmt* s = nullptr;
        bool firstSheet = true;
        if (sqlite3_prepare_v2(db_,
                "SELECT id,name,xmin,xmax,ymin,ymax,degrees,selected,next_eqid"
                " FROM sheets ORDER BY ord,id;", -1, &s, nullptr) == SQLITE_OK) {
            while (sqlite3_step(s) == SQLITE_ROW) {
                int64_t sid = sqlite3_column_int64(s, 0);
                const unsigned char* nm = sqlite3_column_text(s, 1);
                o += firstSheet ? "\n    {" : ",\n    {";
                firstSheet = false;
                o += "\n      \"name\": \"" + json::escape(nm ? reinterpret_cast<const char*>(nm) : "") + "\",";
                o += "\n      \"xmin\": " + d2s(sqlite3_column_double(s, 2)) +
                     ", \"xmax\": " + d2s(sqlite3_column_double(s, 3)) +
                     ", \"ymin\": " + d2s(sqlite3_column_double(s, 4)) +
                     ", \"ymax\": " + d2s(sqlite3_column_double(s, 5)) + ",";
                o += "\n      \"degrees\": " + std::string(sqlite3_column_int(s, 6) ? "true" : "false") +
                     ", \"selected\": " + std::to_string(sqlite3_column_int(s, 7)) +
                     ", \"nextEqId\": " + std::to_string(sqlite3_column_int(s, 8)) + ",";
                o += "\n      \"equations\": [";

                sqlite3_stmt* e = nullptr;
                bool firstEq = true;
                if (sqlite3_prepare_v2(db_,
                        "SELECT type,expr,exprx,expry,cr,cg,cb,ca,enabled,tmin,tmax,tstep"
                        " FROM equations WHERE sheet_id=? ORDER BY slot;",
                        -1, &e, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(e, 1, sid);
                    auto etxt = [&](int c) {
                        const unsigned char* t = sqlite3_column_text(e, c);
                        return std::string(t ? reinterpret_cast<const char*>(t) : "");
                    };
                    while (sqlite3_step(e) == SQLITE_ROW) {
                        o += firstEq ? "\n        {" : ",\n        {";
                        firstEq = false;
                        o += "\"type\": " + std::to_string(sqlite3_column_int(e, 0)) +
                             ", \"expr\": \"" + json::escape(etxt(1)) + "\"" +
                             ", \"exprX\": \"" + json::escape(etxt(2)) + "\"" +
                             ", \"exprY\": \"" + json::escape(etxt(3)) + "\"";
                        o += ", \"color\": [" + d2s(sqlite3_column_double(e, 4)) + ", " +
                             d2s(sqlite3_column_double(e, 5)) + ", " +
                             d2s(sqlite3_column_double(e, 6)) + ", " +
                             d2s(sqlite3_column_double(e, 7)) + "]";
                        o += ", \"enabled\": " + std::string(sqlite3_column_int(e, 8) ? "true" : "false") +
                             ", \"tmin\": " + d2s(sqlite3_column_double(e, 9)) +
                             ", \"tmax\": " + d2s(sqlite3_column_double(e, 10)) +
                             ", \"tstep\": " + d2s(sqlite3_column_double(e, 11)) + "}";
                    }
                    sqlite3_finalize(e);
                }
                o += firstEq ? "]" : "\n      ]";
                o += "\n    }";
            }
            sqlite3_finalize(s);
        }
    }
    o += "\n  ]\n}\n";
    return o;
}

bool Store::importJson(const std::string& jsonStr) {
    if (!opened_) return false;
    json::Value root;
    if (!json::parse(jsonStr, root) || root.type != json::Value::Obj) return false;
    const json::Value* sheets = root.find("sheets");
    if (!sheets || sheets->type != json::Value::Arr || sheets->arr.empty()) return false;

    exec(db_, "BEGIN IMMEDIATE;");
    exec(db_, "DELETE FROM equations;");
    exec(db_, "DELETE FROM sheets;");
    exec(db_, "DELETE FROM sqlite_sequence WHERE name='sheets' OR name='equations';");

    int     ord     = 0;
    int64_t firstId = 0;
    for (const auto& sh : sheets->arr) {
        if (sh.type != json::Value::Obj) continue;
        auto num = [&](const char* k, double d) {
            const json::Value* v = sh.find(k); return v ? v->numOr(d) : d;
        };
        std::string name = sh.find("name") ? sh.find("name")->strOr("Sheet") : "Sheet";

        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO sheets(name,ord,xmin,xmax,ymin,ymax,degrees,selected,next_eqid)"
                " VALUES(?,?,?,?,?,?,?,?,?);", -1, &s, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_text  (s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int   (s, 2, ord++);
        sqlite3_bind_double(s, 3, num("xmin", -10));
        sqlite3_bind_double(s, 4, num("xmax", 10));
        sqlite3_bind_double(s, 5, num("ymin", -6));
        sqlite3_bind_double(s, 6, num("ymax", 6));
        sqlite3_bind_int   (s, 7, (sh.find("degrees") && sh.find("degrees")->boolOr(false)) ? 1 : 0);
        sqlite3_bind_int   (s, 8, static_cast<int>(num("selected", 0)));
        sqlite3_bind_int   (s, 9, static_cast<int>(num("nextEqId", 1)));
        sqlite3_step(s);
        sqlite3_finalize(s);
        int64_t sid = sqlite3_last_insert_rowid(db_);
        if (!firstId) firstId = sid;

        const json::Value* eqs = sh.find("equations");
        if (!eqs || eqs->type != json::Value::Arr) continue;
        int slot = 0;
        for (const auto& ev : eqs->arr) {
            if (ev.type != json::Value::Obj) continue;
            auto enr = [&](const char* k, double d) {
                const json::Value* v = ev.find(k); return v ? v->numOr(d) : d;
            };
            auto est = [&](const char* k) {
                const json::Value* v = ev.find(k); return v ? v->strOr("") : std::string();
            };
            float cr = 0.6f, cg = 0.6f, cb = 0.6f, ca = 1.0f;
            const json::Value* col = ev.find("color");
            if (col && col->type == json::Value::Arr && col->arr.size() >= 4) {
                cr = static_cast<float>(col->arr[0].numOr(cr));
                cg = static_cast<float>(col->arr[1].numOr(cg));
                cb = static_cast<float>(col->arr[2].numOr(cb));
                ca = static_cast<float>(col->arr[3].numOr(ca));
            }
            sqlite3_stmt* e = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "INSERT INTO equations(sheet_id,slot,eqid,type,expr,exprx,expry,"
                    "cr,cg,cb,ca,enabled,tmin,tmax,tstep)"
                    " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);",
                    -1, &e, nullptr) != SQLITE_OK) continue;
            sqlite3_bind_int64 (e, 1,  sid);
            sqlite3_bind_int   (e, 2,  slot);
            sqlite3_bind_int   (e, 3,  slot + 1);
            sqlite3_bind_int   (e, 4,  static_cast<int>(enr("type", 0)));
            std::string ex = est("expr"), exx = est("exprX"), exy = est("exprY");
            sqlite3_bind_text  (e, 5,  ex.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (e, 6,  exx.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text  (e, 7,  exy.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(e, 8,  cr);
            sqlite3_bind_double(e, 9,  cg);
            sqlite3_bind_double(e, 10, cb);
            sqlite3_bind_double(e, 11, ca);
            bool en = ev.find("enabled") ? ev.find("enabled")->boolOr(true) : true;
            sqlite3_bind_int   (e, 12, en ? 1 : 0);
            sqlite3_bind_double(e, 13, enr("tmin", 0));
            sqlite3_bind_double(e, 14, enr("tmax", 6.28318530717958648));
            sqlite3_bind_double(e, 15, enr("tstep", 0));
            sqlite3_step(e);
            sqlite3_finalize(e);
            slot++;
        }
    }
    if (firstId) setApp(db_, "active_sheet", std::to_string(firstId));
    exec(db_, "COMMIT;");
    return firstId != 0;
}

}  // namespace db
