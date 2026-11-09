#include "admin_db.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <cstdlib>
#include <string>

using nlohmann::json;

static bool parsePortRange(const std::string& value, int& lo, int& hi) {
    if (value.empty()) return false;
    size_t dash = value.find('-');
    if (dash == std::string::npos) {
        lo = hi = atoi(value.c_str());
    } else {
        lo = atoi(value.substr(0, dash).c_str());
        hi = atoi(value.substr(dash + 1).c_str());
    }
    return lo >= 1 && hi <= 65535 && lo <= hi;
}

bool AdminDb::dictPorts(const std::string& q, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json arr = json::array();
    std::string sql = "SELECT id,port_start,port_end,name,description,enabled FROM port_dict";
    if (!q.empty()) sql += " WHERE CAST(port_start AS TEXT) LIKE ? OR name LIKE ?";
    sql += " ORDER BY port_start,port_end";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        int lo = sqlite3_column_int(st, 1), hi = sqlite3_column_int(st, 2);
        json j;
        j["id"] = sqlite3_column_int64(st, 0);
        j["value"] = lo == hi ? std::to_string(lo) : std::to_string(lo) + "-" + std::to_string(hi);
        j["name"] = (const char*)sqlite3_column_text(st, 3);
        j["description"] = (const char*)sqlite3_column_text(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::dictPortAdd(const std::string& value, const std::string& name, const std::string& desc,
                          long long& out_id, std::string& err) {
    int lo = 0, hi = 0;
    if (!parsePortRange(value, lo, hi)) { err = "invalid port or range"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM port_dict WHERE port_start=? AND port_end=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error"; return false;
    }
    sqlite3_bind_int(st, 1, lo); sqlite3_bind_int(st, 2, hi);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out_id = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return true;
    }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db_, "INSERT INTO port_dict(port_start,port_end,name,description,enabled,created_ms,updated_ms) VALUES(?,?,?,?,1,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int(st, 1, lo); sqlite3_bind_int(st, 2, hi);
    sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, nowMs()); sqlite3_bind_int64(st, 6, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { err = "insert failed"; return false; }
    out_id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::dictTtys(const std::string& q, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json arr = json::array();
    std::string sql = "SELECT id,pattern,name,description,kind,enabled FROM tty_dict";
    if (!q.empty()) sql += " WHERE pattern LIKE ? OR name LIKE ?";
    sql += " ORDER BY pattern";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = sqlite3_column_int64(st, 0);
        j["value"] = (const char*)sqlite3_column_text(st, 1);
        j["name"] = (const char*)sqlite3_column_text(st, 2);
        j["description"] = (const char*)sqlite3_column_text(st, 3);
        j["kind"] = (const char*)sqlite3_column_text(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::dictTtyAdd(const std::string& pattern, const std::string& name, const std::string& desc,
                         long long& out_id, std::string& err) {
    if (pattern.empty()) { err = "empty tty"; return false; }
    for (char c : pattern) {
        if (!(isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '*' || c == '?')) {
            err = "invalid tty pattern"; return false;
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM tty_dict WHERE pattern=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error"; return false;
    }
    sqlite3_bind_text(st, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        out_id = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return true;
    }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db_, "INSERT INTO tty_dict(pattern,name,description,kind,enabled,created_ms,updated_ms) VALUES(?,?,?,?,1,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, "serial", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, nowMs()); sqlite3_bind_int64(st, 6, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { err = "insert failed"; return false; }
    out_id = sqlite3_last_insert_rowid(db_);
    return true;
}
