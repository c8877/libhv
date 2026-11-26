// 自愈规则（playbook）：一条「告警 → 命令」的应答规则。
//
// 为什么规则不放内存：改完要立刻生效，而且重启后还得在。规则本身没有执行能力，
// 只是「匹配到告警就生成一个 operation」，是否真的动手由 ops 的审批闸门决定。
//
// actor 用 "playbook:<id>:<name>"：既能一眼看出是规则触发的，又能在 SQL 里按 id 前缀
// 统计频率（`actor LIKE 'playbook:7:%'`），不必给 operations 加列。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <sqlite3.h>

using nlohmann::json;

namespace {

void bindText(sqlite3_stmt* st, int idx, const std::string& v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

const char* kCols =
    "id,name,event,devid_glob,argv,verify_argv,risk,auto_apply,cooldown_s,max_per_hour,note,"
    "enabled,created_ms,updated_ms";

json rowJson(sqlite3_stmt* st) {
    json j;
    j["id"] = (long long)sqlite3_column_int64(st, 0);
    j["name"] = dbColText(st, 1);
    j["event"] = dbColText(st, 2);
    j["devid_glob"] = dbColText(st, 3);
    std::string argv = dbColText(st, 4);
    json a = json::parse(argv, nullptr, false);
    j["argv"] = (a.is_discarded() || !a.is_array()) ? json::array() : a;
    std::string verify = dbColText(st, 5);
    json v = json::parse(verify, nullptr, false);
    j["verify_argv"] = (v.is_discarded() || !v.is_array()) ? json::array() : v;
    j["risk"] = (int)sqlite3_column_int(st, 6);
    j["auto_apply"] = sqlite3_column_int(st, 7) != 0;
    j["cooldown_s"] = (int)sqlite3_column_int(st, 8);
    j["max_per_hour"] = (int)sqlite3_column_int(st, 9);
    j["note"] = dbColText(st, 10);
    j["enabled"] = sqlite3_column_int(st, 11) != 0;
    j["created_ms"] = (long long)sqlite3_column_int64(st, 12);
    j["updated_ms"] = (long long)sqlite3_column_int64(st, 13);
    // 供 UI 显示的 actor 前缀，避免前端自己拼
    j["actor"] = "playbook:" + std::to_string(j["id"].get<long long>()) + ":" + j["name"].get<std::string>();
    return j;
}

bool validName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (unsigned char c : n) {
        if (c < 0x20 || c == '/' || c == '\\') return false;
    }
    return true;
}

} // namespace

bool AdminDb::playbookList(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kCols + " FROM playbooks ORDER BY id";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) arr.push_back(rowJson(st));
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    json_out = out.dump();
    return true;
}

bool AdminDb::playbookGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kCols + " FROM playbooks WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        json_out = rowJson(st).dump();
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::playbookSave(long long id, const std::string& name, const std::string& event,
                           const std::string& devid_glob, const std::string& argv_json,
                           const std::string& verify_json, int risk, bool auto_apply, int cooldown_s,
                           int max_per_hour, const std::string& note, bool enabled, long long& out_id,
                           std::string& err) {
    out_id = 0;
    if (!validName(name)) {
        err = "名称需为 1-64 字节且不能含路径分隔符";
        return false;
    }
    if (note.size() > 256) {
        err = "备注过长";
        return false;
    }
    json argv = json::parse(argv_json, nullptr, false);
    if (argv.is_discarded() || !argv.is_array() || argv.empty() || argv.size() > 64) {
        err = "argv 必须是非空字符串数组（最多 64 段）";
        return false;
    }
    for (auto& v : argv) {
        if (!v.is_string() || v.get<std::string>().empty()) {
            err = "argv 每段都必须是非空字符串";
            return false;
        }
    }
    if (!verify_json.empty()) {
        json v = json::parse(verify_json, nullptr, false);
        if (v.is_discarded() || !v.is_array() || v.empty() || v.size() > 64) {
            err = "verify_argv 必须是非空字符串数组";
            return false;
        }
        for (auto& x : v) {
            if (!x.is_string() || x.get<std::string>().empty()) {
                err = "verify_argv 每段都必须是非空字符串";
                return false;
            }
        }
    }
    if (risk < 1) risk = 1;
    if (risk > 3) risk = 3;
    if (cooldown_s < 0) cooldown_s = 0;
    if (cooldown_s > 86400) cooldown_s = 86400;
    if (max_per_hour < 0) max_per_hour = 0;
    if (max_per_hour > 1000) max_per_hour = 1000;

    std::string ev = event.empty() ? "*" : event;
    std::string glob = devid_glob.empty() ? "*" : devid_glob;

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    long long now = nowMs();
    if (id <= 0) {
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO playbooks(name,event,devid_glob,argv,verify_argv,risk,auto_apply,"
                "cooldown_s,max_per_hour,note,enabled,created_ms,updated_ms) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            err = "db error";
            return false;
        }
        bindText(st, 1, name);
        bindText(st, 2, ev);
        bindText(st, 3, glob);
        bindText(st, 4, argv.dump());
        bindText(st, 5, verify_json);
        sqlite3_bind_int(st, 6, risk);
        sqlite3_bind_int(st, 7, auto_apply ? 1 : 0);
        sqlite3_bind_int(st, 8, cooldown_s);
        sqlite3_bind_int(st, 9, max_per_hour);
        bindText(st, 10, note);
        sqlite3_bind_int(st, 11, enabled ? 1 : 0);
        sqlite3_bind_int64(st, 12, now);
        sqlite3_bind_int64(st, 13, now);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) err = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
        if (!ok) {
            if (err.find("UNIQUE") != std::string::npos) err = "规则名已存在";
            return false;
        }
        out_id = (long long)sqlite3_last_insert_rowid(db_);
        return true;
    }

    if (sqlite3_prepare_v2(db_,
            "UPDATE playbooks SET name=?,event=?,devid_glob=?,argv=?,verify_argv=?,risk=?,"
            "auto_apply=?,cooldown_s=?,max_per_hour=?,note=?,enabled=?,updated_ms=? WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    bindText(st, 1, name);
    bindText(st, 2, ev);
    bindText(st, 3, glob);
    bindText(st, 4, argv.dump());
    bindText(st, 5, verify_json);
    sqlite3_bind_int(st, 6, risk);
    sqlite3_bind_int(st, 7, auto_apply ? 1 : 0);
    sqlite3_bind_int(st, 8, cooldown_s);
    sqlite3_bind_int(st, 9, max_per_hour);
    bindText(st, 10, note);
    sqlite3_bind_int(st, 11, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 12, now);
    sqlite3_bind_int64(st, 13, id);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    int changed = done ? sqlite3_changes(db_) : 0;
    std::string e = done ? std::string() : sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!done) {
        err = e.find("UNIQUE") != std::string::npos ? "规则名已存在" : e;
        return false;
    }
    if (changed == 0) {
        err = "规则不存在";
        return false;
    }
    out_id = id;
    return true;
}

bool AdminDb::playbookDelete(long long id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM playbooks WHERE id=?", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    int changed = done ? sqlite3_changes(db_) : 0;
    sqlite3_finalize(st);
    return done && changed > 0;
}

bool AdminDb::playbookSetEnabled(long long id, bool enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE playbooks SET enabled=?,updated_ms=? WHERE id=?", -1, &st,
                           nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 2, nowMs());
    sqlite3_bind_int64(st, 3, id);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    int changed = done ? sqlite3_changes(db_) : 0;
    sqlite3_finalize(st);
    return done && changed > 0;
}

long long AdminDb::playbookCountRecent(long long id, long long since_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string like = "playbook:" + std::to_string(id) + ":%";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM operations WHERE actor LIKE ? AND created_ms>=? AND status IN "
            "('running','applied','verified','failed')",
            -1, &st, nullptr) != SQLITE_OK)
        return -1;
    bindText(st, 1, like);
    sqlite3_bind_int64(st, 2, since_ms);
    long long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = (long long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

long long AdminDb::playbookCountRecentAll(long long since_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM operations WHERE actor LIKE 'playbook:%' AND created_ms>=? AND "
            "status IN ('running','applied','verified','failed')",
            -1, &st, nullptr) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, since_ms);
    long long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = (long long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

bool AdminDb::playbookFires(int limit, std::string& json_out) {
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "SELECT id,actor,status,risk,target_id,plan,error,created_ms,updated_ms,result "
                      "FROM operations WHERE actor LIKE 'playbook:%' ORDER BY id DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, limit);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["op_id"] = (long long)sqlite3_column_int64(st, 0);
        std::string actor = dbColText(st, 1);
        j["actor"] = actor;
        // playbook:<id>:<name> → 拆出来方便页面按规则分组
        size_t a = actor.find(':');
        size_t b = a == std::string::npos ? std::string::npos : actor.find(':', a + 1);
        if (b != std::string::npos) {
            j["playbook_id"] = atoll(actor.substr(a + 1, b - a - 1).c_str());
            j["playbook"] = actor.substr(b + 1);
        }
        j["status"] = dbColText(st, 2);
        j["risk"] = (int)sqlite3_column_int(st, 3);
        j["devid"] = dbColText(st, 4);
        json plan = json::parse(dbColText(st, 5), nullptr, false);
        if (!plan.is_discarded() && plan.contains("argv")) j["argv"] = plan["argv"];
        std::string err = dbColText(st, 6);
        if (!err.empty()) j["error"] = err;
        j["created_ms"] = (long long)sqlite3_column_int64(st, 7);
        j["updated_ms"] = (long long)sqlite3_column_int64(st, 8);
        std::string res = dbColText(st, 9);
        if (!res.empty()) {
            json r = json::parse(res, nullptr, false);
            j["result"] = r.is_discarded() ? json(res) : r;
        }
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    json_out = out.dump();
    return true;
}
