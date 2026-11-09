#include "admin_db.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <cstdlib>
#include <string>

using nlohmann::json;

static int portRefs(sqlite3* db, long long id);
static int ttyRefs(sqlite3* db, long long id);

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
        j["value"] = (lo == 1 && hi == 65535) ? "*"
            : (lo == hi ? std::to_string(lo) : std::to_string(lo) + "-" + std::to_string(hi));
        j["name"] = (const char*)sqlite3_column_text(st, 3);
        j["description"] = (const char*)sqlite3_column_text(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        j["refs"] = portRefs(db_, j["id"].get<long long>());
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
        j["refs"] = ttyRefs(db_, j["id"].get<long long>());
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

static int refCount(sqlite3* db, const char* acl_sql, const char* cap_sql, long long id) {
    int n = 0;
    const char* queries[] = {acl_sql, cap_sql};
    for (auto* q : queries) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, q, -1, &st, nullptr) != SQLITE_OK) continue;
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) n += sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static int portRefs(sqlite3* db, long long id) {
    return refCount(db,
                    "SELECT COUNT(*) FROM client_acl_ports WHERE port_id=?",
                    "SELECT COUNT(*) FROM agent_group_ports WHERE port_id=?", id);
}

static int ttyRefs(sqlite3* db, long long id) {
    return refCount(db,
                    "SELECT COUNT(*) FROM client_acl_ttys WHERE tty_id=?",
                    "SELECT COUNT(*) FROM agent_group_ttys WHERE tty_id=?", id);
}

bool AdminDb::dictBatchAdd(const std::string& kind, const std::vector<std::string>& values,
                           const std::string& name_prefix, std::string& json_out, std::string& err) {
    bool is_port = kind == "ports";
    json added = json::array(), existing = json::array(), invalid = json::array();
    std::set<std::string> seen;
    std::vector<std::pair<std::string, int>> parsed;
    for (auto v : values) {
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        if (v.empty()) continue;
        if (!seen.insert(v).second) continue;
        if (is_port) {
            int lo = 0, hi = 0;
            if (!parsePortRange(v, lo, hi)) { invalid.push_back(v); continue; }
            parsed.push_back({v, 1});
        } else {
            bool ok = !v.empty();
            for (char c : v) if (!(isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '*' || c == '?')) ok = false;
            if (!ok) { invalid.push_back(v); continue; }
            parsed.push_back({v, 0});
        }
    }
    if (!invalid.empty()) {
        json out; out["invalid"] = invalid; json_out = out.dump();
        err = "invalid values"; return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    for (auto& pv : parsed) {
        const std::string& value = pv.first;
        std::string name = name_prefix.empty() ? value : name_prefix + " " + value;
        sqlite3_stmt* st = nullptr;
        if (is_port) {
            int lo = 0, hi = 0; parsePortRange(value, lo, hi);
            if (sqlite3_prepare_v2(db_, "SELECT id FROM port_dict WHERE port_start=? AND port_end=?", -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e); err = "db error"; return false;
            }
            sqlite3_bind_int(st, 1, lo); sqlite3_bind_int(st, 2, hi);
            if (sqlite3_step(st) == SQLITE_ROW) {
                existing.push_back(value); sqlite3_finalize(st); continue;
            }
            sqlite3_finalize(st);
            if (sqlite3_prepare_v2(db_, "INSERT INTO port_dict(port_start,port_end,name,description,enabled,created_ms,updated_ms) VALUES(?,?,?,?,1,?,?)",
                                   -1, &st, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); err = "db error"; return false; }
            sqlite3_bind_int(st, 1, lo); sqlite3_bind_int(st, 2, hi);
            sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, "", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 5, nowMs()); sqlite3_bind_int64(st, 6, nowMs());
            if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); exec("ROLLBACK", e); err = "insert failed"; return false; }
            sqlite3_finalize(st);
        } else {
            if (sqlite3_prepare_v2(db_, "SELECT id FROM tty_dict WHERE pattern=?", -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e); err = "db error"; return false;
            }
            sqlite3_bind_text(st, 1, value.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                existing.push_back(value); sqlite3_finalize(st); continue;
            }
            sqlite3_finalize(st);
            if (sqlite3_prepare_v2(db_, "INSERT INTO tty_dict(pattern,name,description,kind,enabled,created_ms,updated_ms) VALUES(?,?,?,?,1,?,?)",
                                   -1, &st, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); err = "db error"; return false; }
            sqlite3_bind_text(st, 1, value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, "", -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 4, "serial", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 5, nowMs()); sqlite3_bind_int64(st, 6, nowMs());
            if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); exec("ROLLBACK", e); err = "insert failed"; return false; }
            sqlite3_finalize(st);
        }
        added.push_back(value);
    }
    exec("COMMIT", e);
    json out; out["added"] = added; out["existing"] = existing;
    json_out = out.dump();
    return true;
}

bool AdminDb::dictUpdate(const std::string& kind, long long id, const std::string& name,
                         const std::string& desc, int enabled, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = kind == "ports"
        ? "UPDATE port_dict SET name=?,description=?,enabled=?,updated_ms=? WHERE id=?"
        : "UPDATE tty_dict SET name=?,description=?,enabled=?,updated_ms=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 4, nowMs());
    sqlite3_bind_int64(st, 5, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) err = "update failed";
    return ok;
}

bool AdminDb::dictDelete(const std::string& kind, long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (kind == "ports") {
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM port_dict WHERE id=?", -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "not found"; return false; }
        sqlite3_finalize(st);
        if (portRefs(db_, id) > 0) { err = "referenced"; return false; }
        if (sqlite3_prepare_v2(db_, "DELETE FROM port_dict WHERE id=?", -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    } else {
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM tty_dict WHERE id=?", -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "not found"; return false; }
        sqlite3_finalize(st);
        if (ttyRefs(db_, id) > 0) { err = "referenced"; return false; }
        if (sqlite3_prepare_v2(db_, "DELETE FROM tty_dict WHERE id=?", -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    }
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) err = "delete failed";
    return ok;
}

bool AdminDb::dictBatchDelete(const std::string& kind, const std::vector<long long>& ids,
                              std::string& json_out) {
    json deleted = json::array(), skipped = json::array();
    for (long long id : ids) {
        std::string err;
        if (dictDelete(kind, id, err)) deleted.push_back(id);
        else skipped.push_back({{"id", id}, {"error", err}});
    }
    json out; out["deleted"] = deleted; out["skipped"] = skipped;
    json_out = out.dump();
    return true;
}
