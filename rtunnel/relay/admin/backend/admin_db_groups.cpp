#include "admin_db.hpp"
#include <utility>
#include <map>
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <set>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using nlohmann::json;

namespace {

const char* kCaps[] = {"tcp_ports", "shell", "tty", "file", "sysinfo", "metrics", "config", "web"};

std::string groupCapsJson(sqlite3* db, long long id) {
    json caps = json::object();
    for (auto* c : kCaps) caps[c] = (std::string(c) == "file") ? "none" : "";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT cap,value FROM dev_group_caps WHERE group_id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            caps[(const char*)sqlite3_column_text(st, 0)] = (const char*)sqlite3_column_text(st, 1);
        }
    }
    sqlite3_finalize(st);
    return caps.dump();
}

} // namespace

bool AdminDb::groupGet(long long id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,name,description,created_ms,updated_ms "
                               "FROM dev_groups WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "group not found";
        return false;
    }
    json g;
    g["id"] = sqlite3_column_int64(st, 0);
    g["name"] = (const char*)sqlite3_column_text(st, 1);
    g["description"] = (const char*)sqlite3_column_text(st, 2);
    g["created_ms"] = sqlite3_column_int64(st, 3);
    g["updated_ms"] = sqlite3_column_int64(st, 4);
    sqlite3_finalize(st);

    g["devices"] = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid FROM dev_group_members WHERE group_id=? ORDER BY devid",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) g["devices"].push_back((const char*)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);

    g["subjects"] = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT subject_type,subject_id FROM subject_groups "
                               "WHERE group_id=? ORDER BY subject_type,subject_id",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            json s;
            s["type"] = (const char*)sqlite3_column_text(st, 0);
            s["id"] = (const char*)sqlite3_column_text(st, 1);
            g["subjects"].push_back(s);
        }
    }
    sqlite3_finalize(st);
    g["caps"] = json::parse(groupCapsJson(db_, id), nullptr, false);
    json_out = g.dump();
    return true;
}

bool AdminDb::groupDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM subject_groups WHERE group_id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0) {
            sqlite3_finalize(st);
            err = "group has subjects";
            return false;
        }
    }
    sqlite3_finalize(st);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    bool ok = exec(("DELETE FROM dev_group_members WHERE group_id=" + std::to_string(id)).c_str(), e) &&
              exec(("DELETE FROM dev_group_caps WHERE group_id=" + std::to_string(id)).c_str(), e) &&
              exec(("DELETE FROM dev_groups WHERE id=" + std::to_string(id)).c_str(), e);
    exec(ok ? "COMMIT" : "ROLLBACK", e);
    if (!ok) err = e.empty() ? "delete failed" : e;
    return ok;
}

bool AdminDb::groupDeviceMap(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json out = json::object();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT m.devid,g.name FROM dev_group_members m "
                               "JOIN dev_groups g ON g.id=m.group_id ORDER BY m.devid,g.name",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* devid = (const char*)sqlite3_column_text(st, 0);
        out[devid].push_back((const char*)sqlite3_column_text(st, 1));
    }
    sqlite3_finalize(st);
    json_out = out.dump();
    return true;
}

bool AdminDb::groupSubjectDevids(const std::string& type, const std::string& subject,
                                 std::vector<std::string>& out, bool& has_groups) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    has_groups = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM subject_groups WHERE subject_type=? AND subject_id=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) has_groups = sqlite3_column_int64(st, 0) > 0;
    sqlite3_finalize(st);
    if (!has_groups) return true;
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT m.devid FROM dev_group_members m "
                               "JOIN subject_groups s ON s.group_id=m.group_id "
                               "WHERE s.subject_type=? AND s.subject_id=? ORDER BY m.devid",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::userGroupList(const std::string& username, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json groups = json::array();
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT g.id,g.name,"
        "(SELECT COUNT(*) FROM dev_group_members m WHERE m.group_id=g.id),"
        "EXISTS(SELECT 1 FROM subject_groups s WHERE s.group_id=g.id AND s.subject_type='admin' AND s.subject_id=?) "
        "FROM dev_groups g ORDER BY g.name";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        json g;
        g["id"] = sqlite3_column_int64(st, 0);
        g["name"] = (const char*)sqlite3_column_text(st, 1);
        g["device_count"] = sqlite3_column_int64(st, 2);
        g["selected"] = sqlite3_column_int(st, 3) != 0;
        groups.push_back(g);
    }
    sqlite3_finalize(st);
    json out;
    out["groups"] = groups;
    json_out = out.dump();
    return true;
}

bool AdminDb::userSetGroups(const std::string& username, const std::vector<long long>& ids, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM subject_groups WHERE subject_type='admin' AND subject_id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e); err = "db error"; return false;
    }
    sqlite3_bind_text(st, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { exec("ROLLBACK", e); err = "delete failed"; return false; }
    for (long long id : ids) {
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM dev_groups WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e); err = "db error"; return false;
        }
        sqlite3_bind_int64(st, 1, id);
        bool exists = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (!exists) { exec("ROLLBACK", e); err = "group not found"; return false; }
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO subject_groups(subject_type,subject_id,group_id,added_ms) "
                                   "VALUES('admin',?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e); err = "db error"; return false;
        }
        sqlite3_bind_text(st, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_bind_int64(st, 3, nowMs());
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { exec("ROLLBACK", e); err = "insert failed"; return false; }
    }
    if (!exec("COMMIT", e)) { exec("ROLLBACK", e); err = e; return false; }
    return true;
}
