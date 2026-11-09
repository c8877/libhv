#include "admin_db.hpp"
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

bool validCap(const std::string& cap) {
    for (auto* c : kCaps) if (cap == c) return true;
    return false;
}

bool truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

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

bool AdminDb::groupList(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json arr = json::array();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,name,description,created_ms,updated_ms "
                               "FROM dev_groups ORDER BY name", -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(st, 0);
        json g;
        g["id"] = id;
        g["name"] = (const char*)sqlite3_column_text(st, 1);
        g["description"] = (const char*)sqlite3_column_text(st, 2);
        g["created_ms"] = sqlite3_column_int64(st, 3);
        g["updated_ms"] = sqlite3_column_int64(st, 4);
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM dev_group_members WHERE group_id=?",
                               -1, &c, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(c, 1, id);
            if (sqlite3_step(c) == SQLITE_ROW) g["device_count"] = sqlite3_column_int64(c, 0);
        }
        sqlite3_finalize(c);
        c = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM subject_groups WHERE group_id=?",
                               -1, &c, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(c, 1, id);
            if (sqlite3_step(c) == SQLITE_ROW) g["subject_count"] = sqlite3_column_int64(c, 0);
        }
        sqlite3_finalize(c);
        g["caps"] = json::parse(groupCapsJson(db_, id), nullptr, false);
        arr.push_back(g);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

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

bool AdminDb::groupCreate(const std::string& name, const std::string& desc,
                          long long& out_id, std::string& err) {
    if (name.empty() || name.size() > 64) { err = "invalid group name"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    long long now = nowMs();
    if (sqlite3_prepare_v2(db_, "INSERT INTO dev_groups(name,description,created_ms,updated_ms)"
                               " VALUES(?,?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = (rc == SQLITE_CONSTRAINT) ? "group name exists" : sqlite3_errmsg(db_); return false; }
    out_id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::groupUpdate(long long id, const std::string& name, const std::string& desc,
                          std::string& err) {
    if (name.empty() || name.size() > 64) { err = "invalid group name"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE dev_groups SET name=?,description=?,updated_ms=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, nowMs());
    sqlite3_bind_int64(st, 4, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = (rc == SQLITE_CONSTRAINT) ? "group name exists" : sqlite3_errmsg(db_); return false; }
    return sqlite3_changes(db_) > 0 || true;
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

bool AdminDb::groupAddDevice(long long id, const std::string& devid, std::string& err) {
    if (devid.empty() || devid.size() > 128) { err = "invalid devid"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO dev_group_members(group_id,devid,added_ms)"
                               " VALUES(?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db_); return false; }
    return true;
}

bool AdminDb::groupRemoveDevice(long long id, const std::string& devid) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM dev_group_members WHERE group_id=? AND devid=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::groupSetCaps(long long id, const std::map<std::string, std::string>& caps,
                           std::string& err) {
    for (auto& kv : caps) if (!validCap(kv.first)) { err = "unknown cap"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    bool ok = exec(("DELETE FROM dev_group_caps WHERE group_id=" + std::to_string(id)).c_str(), e);
    for (auto& kv : caps) {
        if (!ok) break;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT INTO dev_group_caps(group_id,cap,value) VALUES(?,?,?)",
                               -1, &st, nullptr) != SQLITE_OK) {
            ok = false; e = sqlite3_errmsg(db_); break;
        }
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_text(st, 2, kv.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, kv.second.c_str(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) e = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
    }
    if (ok) ok = exec(("UPDATE dev_groups SET updated_ms=" + std::to_string(nowMs()) +
                       " WHERE id=" + std::to_string(id)).c_str(), e);
    exec(ok ? "COMMIT" : "ROLLBACK", e);
    if (!ok) err = e;
    return ok;
}

bool AdminDb::groupAddSubject(long long id, const std::string& type, const std::string& subject,
                              std::string& err) {
    if (type != "admin" && type != "client") { err = "bad subject type"; return false; }
    if (subject.empty() || subject.size() > 64) { err = "bad subject"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    if (type == "admin") {
        sqlite3_stmt* st = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM users WHERE username=?", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
            found = sqlite3_step(st) == SQLITE_ROW;
        }
        sqlite3_finalize(st);
        if (!found) { err = "admin user not found"; return false; }
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO subject_groups(subject_type,subject_id,group_id,added_ms)"
                               " VALUES(?,?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    sqlite3_bind_int64(st, 4, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = sqlite3_errmsg(db_); return false; }
    return true;
}

bool AdminDb::groupRemoveSubject(long long id, const std::string& type, const std::string& subject) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM subject_groups WHERE group_id=? AND subject_type=? AND subject_id=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, subject.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
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

bool AdminDb::groupEffective(const std::string& type, const std::string& subject,
                             std::string& json_out, std::string& err) {
    if (type != "admin" && type != "client") { err = "bad subject type"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    json groups = json::array();
    std::set<std::string> devices;
    std::map<std::string, std::set<std::string>> multi;
    std::map<std::string, std::string> scalar;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT g.id,g.name FROM subject_groups s "
                               "JOIN dev_groups g ON g.id=s.group_id "
                               "WHERE s.subject_type=? AND s.subject_id=? ORDER BY g.name",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<long long> ids;
    while (sqlite3_step(st) == SQLITE_ROW) {
        long long id = sqlite3_column_int64(st, 0);
        ids.push_back(id);
        json g;
        g["id"] = id;
        g["name"] = (const char*)sqlite3_column_text(st, 1);
        groups.push_back(g);
    }
    sqlite3_finalize(st);

    for (long long id : ids) {
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT devid FROM dev_group_members WHERE group_id=?",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            while (sqlite3_step(st) == SQLITE_ROW) devices.insert((const char*)sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT cap,value FROM dev_group_caps WHERE group_id=?",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string cap = (const char*)sqlite3_column_text(st, 0);
                std::string val = (const char*)sqlite3_column_text(st, 1);
                if (cap == "tcp_ports" || cap == "tty") {
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "%s", val.c_str());
                    char* save = nullptr;
                    for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(nullptr, ",", &save))
                        multi[cap].insert(p);
                } else if (cap == "file") {
                    auto rank = [](const std::string& v) { return v == "rw" ? 3 : v == "ro" ? 2 : 1; };
                    if (rank(val) > rank(scalar[cap])) scalar[cap] = val;
                } else if (truthy(val)) {
                    scalar[cap] = "1";
                }
            }
        }
        sqlite3_finalize(st);
    }

    json caps = json::object();
    for (auto* c : kCaps) caps[c] = (std::string(c) == "file") ? "none" : "";
    for (auto& kv : multi) {
        std::string joined;
        for (auto& v : kv.second) { if (!joined.empty()) joined += ","; joined += v; }
        caps[kv.first] = joined;
    }
    for (auto& kv : scalar) caps[kv.first] = kv.second;

    json out;
    out["subject_type"] = type;
    out["subject_id"] = subject;
    out["groups"] = groups;
    out["devices"] = json::array();
    for (auto& d : devices) out["devices"].push_back(d);
    out["caps"] = caps;
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
