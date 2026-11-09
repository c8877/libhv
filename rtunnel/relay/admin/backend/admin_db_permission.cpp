#include "admin_db.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

using nlohmann::json;

namespace {
const char* kCaps[] = {"tcp_ports", "shell", "tty", "file", "sysinfo", "metrics", "config", "web"};
bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
std::string capValue(sqlite3* db, long long group, const char* cap) {
    sqlite3_stmt* st = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, "SELECT value FROM dev_group_caps WHERE group_id=? AND cap=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, group);
        sqlite3_bind_text(st, 2, cap, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) out = (const char*)sqlite3_column_text(st, 0);
    }
    sqlite3_finalize(st);
    return out;
}
json agentCaps(sqlite3* db, long long group) {
    json caps = json::object();
    for (auto* c : kCaps) caps[c] = std::string(c) == "file" ? "none" : "";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT cap,value FROM dev_group_caps WHERE group_id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, group);
        while (sqlite3_step(st) == SQLITE_ROW)
            caps[(const char*)sqlite3_column_text(st, 0)] = (const char*)sqlite3_column_text(st, 1);
    }
    sqlite3_finalize(st);
    return caps;
}
} // namespace

bool AdminDb::permOverview(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json out;
    json cg = json::array();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,name,description,updated_ms FROM client_groups ORDER BY name",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            long long id = sqlite3_column_int64(st, 0);
            json g;
            g["id"] = id;
            g["name"] = (const char*)sqlite3_column_text(st, 1);
            g["description"] = (const char*)sqlite3_column_text(st, 2);
            g["updated_ms"] = sqlite3_column_int64(st, 3);
            sqlite3_stmt* c = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM client_group_members WHERE group_id=?",
                                   -1, &c, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(c, 1, id);
                if (sqlite3_step(c) == SQLITE_ROW) g["user_count"] = sqlite3_column_int64(c, 0);
            }
            sqlite3_finalize(c);
            c = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM client_group_acl WHERE client_group_id=?",
                                   -1, &c, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(c, 1, id);
                if (sqlite3_step(c) == SQLITE_ROW) g["acl_count"] = sqlite3_column_int64(c, 0);
            }
            sqlite3_finalize(c);
            cg.push_back(g);
        }
    }
    sqlite3_finalize(st);

    json ag = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,name,description,updated_ms FROM dev_groups ORDER BY name",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            long long id = sqlite3_column_int64(st, 0);
            json g;
            g["id"] = id;
            g["name"] = (const char*)sqlite3_column_text(st, 1);
            g["description"] = (const char*)sqlite3_column_text(st, 2);
            g["updated_ms"] = sqlite3_column_int64(st, 3);
            g["caps"] = agentCaps(db_, id);
            sqlite3_stmt* c = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM dev_group_members WHERE group_id=?",
                                   -1, &c, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(c, 1, id);
                if (sqlite3_step(c) == SQLITE_ROW) g["device_count"] = sqlite3_column_int64(c, 0);
            }
            sqlite3_finalize(c);
            c = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM client_group_acl WHERE agent_group_id=?",
                                   -1, &c, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(c, 1, id);
                if (sqlite3_step(c) == SQLITE_ROW) g["ref_count"] = sqlite3_column_int64(c, 0);
            }
            sqlite3_finalize(c);
            ag.push_back(g);
        }
    }
    sqlite3_finalize(st);
    out["client_groups"] = cg;
    out["agent_groups"] = ag;
    json_out = out.dump();
    return true;
}

bool AdminDb::permClientGet(long long id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,name,description,created_ms,updated_ms FROM client_groups WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "group not found"; return false; }
    json g;
    g["id"] = sqlite3_column_int64(st, 0);
    g["name"] = (const char*)sqlite3_column_text(st, 1);
    g["description"] = (const char*)sqlite3_column_text(st, 2);
    g["created_ms"] = sqlite3_column_int64(st, 3);
    g["updated_ms"] = sqlite3_column_int64(st, 4);
    sqlite3_finalize(st);

    g["users"] = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT username FROM client_group_members WHERE group_id=? ORDER BY username",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) g["users"].push_back((const char*)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);

    g["acl"] = json::array();
    st = nullptr;
    const char* sql =
        "SELECT a.agent_group_id,g.name,a.tcp_ports,a.allow_shell,a.tty,a.file,"
        "a.sysinfo,a.metrics,a.config,a.web FROM client_group_acl a "
        "LEFT JOIN dev_groups g ON g.id=a.agent_group_id WHERE a.client_group_id=? ORDER BY g.name";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            json a;
            a["agent_group_id"] = sqlite3_column_int64(st, 0);
            a["agent_group_name"] = sqlite3_column_text(st, 1) ? (const char*)sqlite3_column_text(st, 1) : "";
            a["tcp_ports"] = (const char*)sqlite3_column_text(st, 2);
            a["shell"] = sqlite3_column_int(st, 3) != 0;
            a["tty"] = (const char*)sqlite3_column_text(st, 4);
            a["file"] = (const char*)sqlite3_column_text(st, 5);
            a["sysinfo"] = sqlite3_column_int(st, 6) != 0;
            a["metrics"] = sqlite3_column_int(st, 7) != 0;
            a["config"] = sqlite3_column_int(st, 8) != 0;
            a["web"] = sqlite3_column_int(st, 9) != 0;
            g["acl"].push_back(a);
        }
    }
    sqlite3_finalize(st);
    json_out = g.dump();
    return true;
}

bool AdminDb::permClientSave(long long id, const std::string& name, const std::string& desc,
                             const std::vector<std::string>& users,
                             const std::vector<DbPermAcl>& acl, long long& out_id, std::string& err) {
    if (name.empty() || name.size() > 64) { err = "invalid name"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    sqlite3_stmt* st = nullptr;
    if (id == 0) {
        if (sqlite3_prepare_v2(db_, "INSERT INTO client_groups(name,description,created_ms,updated_ms) VALUES(?,?,?,?)",
                               -1, &st, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); err = sqlite3_errmsg(db_); return false; }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, nowMs());
        sqlite3_bind_int64(st, 4, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { exec("ROLLBACK", e); err = sqlite3_errmsg(db_); return false; }
        id = sqlite3_last_insert_rowid(db_);
    } else {
        if (sqlite3_prepare_v2(db_, "UPDATE client_groups SET name=?,description=?,updated_ms=? WHERE id=?",
                               -1, &st, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); err = "db error"; return false; }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, nowMs());
        sqlite3_bind_int64(st, 4, id);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { exec("ROLLBACK", e); err = sqlite3_errmsg(db_); return false; }
    }
    std::string sid = std::to_string(id);
    bool ok = exec(("DELETE FROM client_group_members WHERE group_id=" + sid).c_str(), e) &&
              exec(("DELETE FROM client_group_acl WHERE client_group_id=" + sid).c_str(), e);
    for (auto& u : users) {
        if (!ok) break;
        if (u.empty()) continue;
        sqlite3_stmt* m = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO client_group_members(group_id,username,added_ms) VALUES(?,?,?)",
                               -1, &m, nullptr) != SQLITE_OK) { ok = false; e = sqlite3_errmsg(db_); break; }
        sqlite3_bind_int64(m, 1, id);
        sqlite3_bind_text(m, 2, u.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(m, 3, nowMs());
        ok = sqlite3_step(m) == SQLITE_DONE;
        sqlite3_finalize(m);
    }
    for (auto& a : acl) {
        if (!ok) break;
        sqlite3_stmt* x = nullptr;
        const char* q = "INSERT OR REPLACE INTO client_group_acl(client_group_id,agent_group_id,tcp_ports,"
                        "allow_shell,tty,file,sysinfo,metrics,config,web,updated_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db_, q, -1, &x, nullptr) != SQLITE_OK) { ok = false; e = sqlite3_errmsg(db_); break; }
        sqlite3_bind_int64(x, 1, id);
        sqlite3_bind_int64(x, 2, a.agent_group_id);
        sqlite3_bind_text(x, 3, a.tcp_ports.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(x, 4, a.shell ? 1 : 0);
        sqlite3_bind_text(x, 5, a.tty.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(x, 6, a.file.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(x, 7, a.sysinfo ? 1 : 0);
        sqlite3_bind_int(x, 8, a.metrics ? 1 : 0);
        sqlite3_bind_int(x, 9, a.config ? 1 : 0);
        sqlite3_bind_int(x, 10, a.web ? 1 : 0);
        sqlite3_bind_int64(x, 11, nowMs());
        ok = sqlite3_step(x) == SQLITE_DONE;
        if (!ok) e = sqlite3_errmsg(db_);
        sqlite3_finalize(x);
    }
    exec(ok ? "COMMIT" : "ROLLBACK", e);
    if (!ok) { err = e; return false; }
    out_id = id;
    return true;
}

bool AdminDb::permClientDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    std::string sid = std::to_string(id);
    bool ok = exec(("DELETE FROM client_group_members WHERE group_id=" + sid).c_str(), e) &&
              exec(("DELETE FROM client_group_acl WHERE client_group_id=" + sid).c_str(), e) &&
              exec(("DELETE FROM client_groups WHERE id=" + sid).c_str(), e);
    exec(ok ? "COMMIT" : "ROLLBACK", e);
    if (!ok) err = e;
    return ok;
}

bool AdminDb::permAgentGet(long long id, std::string& json_out, std::string& err) {
    return groupGet(id, json_out, err);
}

bool AdminDb::permAgentSave(long long id, const std::string& name, const std::string& desc,
                            const std::vector<std::string>& devices,
                            const std::map<std::string, std::string>& caps,
                            long long& out_id, std::string& err) {
    if (name.empty() || name.size() > 64) { err = "invalid name"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    sqlite3_stmt* st = nullptr;
    if (id == 0) {
        if (sqlite3_prepare_v2(db_, "INSERT INTO dev_groups(name,description,created_ms,updated_ms) VALUES(?,?,?,?)",
                               -1, &st, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); err = "db error"; return false; }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, nowMs());
        sqlite3_bind_int64(st, 4, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { exec("ROLLBACK", e); err = sqlite3_errmsg(db_); return false; }
        id = sqlite3_last_insert_rowid(db_);
    } else {
        if (sqlite3_prepare_v2(db_, "UPDATE dev_groups SET name=?,description=?,updated_ms=? WHERE id=?",
                               -1, &st, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); err = "db error"; return false; }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, nowMs());
        sqlite3_bind_int64(st, 4, id);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { exec("ROLLBACK", e); err = sqlite3_errmsg(db_); return false; }
    }
    std::string sid = std::to_string(id);
    bool ok = exec(("DELETE FROM dev_group_members WHERE group_id=" + sid).c_str(), e) &&
              exec(("DELETE FROM dev_group_caps WHERE group_id=" + sid).c_str(), e);
    for (auto& d : devices) {
        if (!ok) break;
        if (d.empty()) continue;
        sqlite3_stmt* m = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO dev_group_members(group_id,devid,added_ms) VALUES(?,?,?)",
                               -1, &m, nullptr) != SQLITE_OK) { ok = false; e = "db error"; break; }
        sqlite3_bind_int64(m, 1, id);
        sqlite3_bind_text(m, 2, d.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(m, 3, nowMs());
        ok = sqlite3_step(m) == SQLITE_DONE;
        sqlite3_finalize(m);
    }
    for (auto& kv : caps) {
        if (!ok) break;
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO dev_group_caps(group_id,cap,value) VALUES(?,?,?)",
                               -1, &c, nullptr) != SQLITE_OK) { ok = false; e = "db error"; break; }
        sqlite3_bind_int64(c, 1, id);
        sqlite3_bind_text(c, 2, kv.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(c, 3, kv.second.c_str(), -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(c) == SQLITE_DONE;
        sqlite3_finalize(c);
    }
    exec(ok ? "COMMIT" : "ROLLBACK", e);
    if (!ok) { err = e; return false; }
    out_id = id;
    return true;
}

bool AdminDb::permAgentDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM client_group_acl WHERE agent_group_id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) > 0) {
            sqlite3_finalize(st);
            err = "agent group is referenced by client groups";
            return false;
        }
    }
    sqlite3_finalize(st);
    return groupDelete(id, err);
}

bool AdminDb::permEffectiveClient(const std::string& user, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    json groups = json::array();
    std::set<std::string> devices;
    std::map<std::string, std::string> caps;
    std::map<std::string, std::string> ports;
    std::map<std::string, std::string> ttys;
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT a.agent_group_id,a.tcp_ports,a.allow_shell,a.tty,a.file,a.sysinfo,a.metrics,a.config,a.web,"
        "g.name FROM client_group_members m "
        "JOIN client_group_acl a ON a.client_group_id=m.group_id "
        "LEFT JOIN dev_groups g ON g.id=a.agent_group_id WHERE m.username=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, user.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        long long gid = sqlite3_column_int64(st, 0);
        groups.push_back(sqlite3_column_text(st, 9) ? (const char*)sqlite3_column_text(st, 9) : "");
        std::string p = (const char*)sqlite3_column_text(st, 1);
        if (!p.empty()) ports[std::to_string(gid)] = p;
        if (sqlite3_column_int(st, 2)) caps["shell"] = "1";
        std::string t = (const char*)sqlite3_column_text(st, 3);
        if (!t.empty()) ttys[std::to_string(gid)] = t;
        std::string f = (const char*)sqlite3_column_text(st, 4);
        auto rank = [](const std::string& v) { return v == "rw" ? 3 : v == "ro" ? 2 : 1; };
        if (rank(f) > rank(caps["file"])) caps["file"] = f;
        if (sqlite3_column_int(st, 5)) caps["sysinfo"] = "1";
        if (sqlite3_column_int(st, 6)) caps["metrics"] = "1";
        if (sqlite3_column_int(st, 7)) caps["config"] = "1";
        if (sqlite3_column_int(st, 8)) caps["web"] = "1";
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT devid FROM dev_group_members WHERE group_id=?", -1, &d, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(d, 1, gid);
            while (sqlite3_step(d) == SQLITE_ROW) devices.insert((const char*)sqlite3_column_text(d, 0));
        }
        sqlite3_finalize(d);
    }
    sqlite3_finalize(st);
    json out;
    out["client"] = user;
    out["groups"] = groups;
    out["devices"] = json::array();
    for (auto& d : devices) out["devices"].push_back(d);
    json c = json::object();
    std::string portJoin, ttyJoin;
    for (auto& kv : ports) { if (!portJoin.empty()) portJoin += ","; portJoin += kv.second; }
    for (auto& kv : ttys) { if (!ttyJoin.empty()) ttyJoin += ","; ttyJoin += kv.second; }
    c["tcp_ports"] = portJoin;
    c["shell"] = caps.count("shell") ? "1" : "0";
    c["tty"] = ttyJoin;
    c["file"] = caps.count("file") ? caps["file"] : "none";
    for (auto* k : {"sysinfo","metrics","config","web"}) c[k] = caps.count(k) ? "1" : "0";
    out["caps"] = c;
    json_out = out.dump();
    return true;
}

bool AdminDb::permMigrateLegacy() {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM client_groups", -1, &st, nullptr) != SQLITE_OK) return false;
    bool empty = false;
    if (sqlite3_step(st) == SQLITE_ROW) empty = sqlite3_column_int64(st, 0) == 0;
    sqlite3_finalize(st);
    if (!empty) return true;

    // 每个存在 client 的旧分组生成一个同名客户端分组，并复制 ACL。
    std::string e;
    if (!exec("BEGIN", e)) return false;
    st = nullptr;
    const char* sql =
        "SELECT DISTINCT g.id,g.name FROM subject_groups s JOIN dev_groups g ON g.id=s.group_id "
        "WHERE s.subject_type='client'";
    std::vector<std::pair<long long, std::string>> groups;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            groups.push_back({sqlite3_column_int64(st, 0), (const char*)sqlite3_column_text(st, 1)});
    }
    sqlite3_finalize(st);
    for (auto& g : groups) {
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT INTO client_groups(name,description,created_ms,updated_ms) VALUES(?,?,?,?)",
                               -1, &c, nullptr) != SQLITE_OK) { exec("ROLLBACK", e); return false; }
        sqlite3_bind_text(c, 1, g.second.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(c, 2, "legacy ACL", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(c, 3, nowMs());
        sqlite3_bind_int64(c, 4, nowMs());
        bool ok = sqlite3_step(c) == SQLITE_DONE;
        sqlite3_finalize(c);
        if (!ok) { exec("ROLLBACK", e); return false; }
        long long cid = sqlite3_last_insert_rowid(db_);
        sqlite3_stmt* m = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO client_group_members(group_id,username,added_ms) "
                                   "SELECT ?,subject_id,? FROM subject_groups WHERE subject_type='client' AND group_id=?",
                               -1, &m, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(m, 1, cid);
            sqlite3_bind_int64(m, 2, nowMs());
            sqlite3_bind_int64(m, 3, g.first);
            sqlite3_step(m);
        }
        sqlite3_finalize(m);
        sqlite3_stmt* a = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO client_group_acl("
                                   "client_group_id,agent_group_id,tcp_ports,allow_shell,tty,file,"
                                   "sysinfo,metrics,config,web,updated_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                               -1, &a, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(a, 1, cid);
            sqlite3_bind_int64(a, 2, g.first);
            std::string ports = capValue(db_, g.first, "tcp_ports");
            std::string tty = capValue(db_, g.first, "tty");
            std::string file = capValue(db_, g.first, "file");
            sqlite3_bind_text(a, 3, ports.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(a, 4, truthy(capValue(db_, g.first, "shell")) ? 1 : 0);
            sqlite3_bind_text(a, 5, tty.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(a, 6, file.empty() ? "none" : file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(a, 7, truthy(capValue(db_, g.first, "sysinfo")) ? 1 : 0);
            sqlite3_bind_int(a, 8, truthy(capValue(db_, g.first, "metrics")) ? 1 : 0);
            sqlite3_bind_int(a, 9, truthy(capValue(db_, g.first, "config")) ? 1 : 0);
            sqlite3_bind_int(a, 10, truthy(capValue(db_, g.first, "web")) ? 1 : 0);
            sqlite3_bind_int64(a, 11, nowMs());
            sqlite3_step(a);
        }
        sqlite3_finalize(a);
    }
    exec("COMMIT", e);
    return true;
}

bool AdminDb::permClientRules(std::vector<DbGroupAclRule>& out, std::set<std::string>& users) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    users.clear();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT username FROM client_group_members", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) users.insert((const char*)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    const char* sql =
        "SELECT m.username,a.agent_group_id,a.tcp_ports,a.allow_shell,a.tty "
        "FROM client_group_members m JOIN client_group_acl a ON a.client_group_id=m.group_id";
    st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string user = (const char*)sqlite3_column_text(st, 0);
        long long gid = sqlite3_column_int64(st, 1);
        std::string ports = (const char*)sqlite3_column_text(st, 2);
        bool shell = sqlite3_column_int(st, 3) != 0;
        std::string tty = (const char*)sqlite3_column_text(st, 4);
        if (!truthy(capValue(db_, gid, "shell"))) shell = false;
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT devid FROM dev_group_members WHERE group_id=?", -1, &d, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(d, 1, gid);
            while (sqlite3_step(d) == SQLITE_ROW) {
                DbGroupAclRule r;
                r.user = user;
                r.devid = (const char*)sqlite3_column_text(d, 0);
                r.ports = ports;
                r.tty = tty;
                r.shell = shell;
                out.push_back(r);
            }
        }
        sqlite3_finalize(d);
    }
    sqlite3_finalize(st);
    return true;
}
