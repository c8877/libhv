// 操作作业（operations）：AI 与人共用的可重放写动作。
//
// 状态机：
//   planned ──apply──────────────▶ running ──▶ applied / failed
//      │                                        │
//      └─risk≥阈值──▶ pending_approval ─获批─▶ approved ─apply─▶ running
//                              └─被拒─▶ rejected
//
// plan 冻结 plan_hash（apply 必须带同一个），(actor,idem_key) 唯一保证重试不重跑。
// 审批必须是**另一个** actor：同一个账号既能提议又能批准等于没有闸门。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <sqlite3.h>

using nlohmann::json;

namespace {

void bindText(sqlite3_stmt* st, int idx, const std::string& v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

json opJson(sqlite3_stmt* st) {
    json j;
    j["id"] = (long long)sqlite3_column_int64(st, 0);
    j["actor"] = dbColText(st, 1);
    j["actor_type"] = dbColText(st, 2);
    j["kind"] = dbColText(st, 3);
    j["target_type"] = dbColText(st, 4);
    j["target_id"] = dbColText(st, 5);
    j["plan"] = json::parse(dbColText(st, 6), nullptr, false);
    if (j["plan"].is_discarded()) j["plan"] = json::object();
    j["plan_hash"] = dbColText(st, 7);
    j["risk"] = (int)sqlite3_column_int(st, 8);
    j["status"] = dbColText(st, 10);
    std::string approver = dbColText(st, 11);
    if (!approver.empty()) {
        j["approver"] = approver;
        j["approved_ms"] = (long long)sqlite3_column_int64(st, 13);
        std::string note = dbColText(st, 12);
        if (!note.empty()) j["approval_note"] = note;
    }
    std::string res = dbColText(st, 14);
    if (!res.empty()) {
        json r = json::parse(res, nullptr, false);
        j["result"] = r.is_discarded() ? json(res) : r;
    }
    std::string err = dbColText(st, 15);
    if (!err.empty()) j["error"] = err;
    j["created_ms"] = (long long)sqlite3_column_int64(st, 16);
    j["updated_ms"] = (long long)sqlite3_column_int64(st, 17);
    return j;
}

const char* kOpCols =
    "id,actor,actor_type,kind,target_type,target_id,plan,plan_hash,risk,idem_key,status,"
    "approver,approval_note,approved_ms,result,error,created_ms,updated_ms";

} // namespace

bool AdminDb::opsCreate(const std::string& actor, const std::string& actor_type, const std::string& kind,
                        const std::string& target_type, const std::string& target_id,
                        const std::string& plan_json, const std::string& plan_hash, int risk,
                        bool need_approval, const std::string& idem_key, long long& out_id,
                        std::string& err) {
    out_id = 0;
    std::lock_guard<std::mutex> lk(mu_);
    if (!idem_key.empty()) {
        // 幂等：同 actor + key 已有作业就直接返回它的 id，不新建
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM operations WHERE actor=? AND idem_key=?",
                               -1, &q, nullptr) == SQLITE_OK) {
            bindText(q, 1, actor);
            bindText(q, 2, idem_key);
            if (sqlite3_step(q) == SQLITE_ROW) out_id = (long long)sqlite3_column_int64(q, 0);
            sqlite3_finalize(q);
        }
        if (out_id > 0) return true;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO operations(actor,actor_type,kind,target_type,target_id,plan,plan_hash,risk,"
            "idem_key,status,created_ms,updated_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    long long now = nowMs();
    bindText(st, 1, actor);
    bindText(st, 2, actor_type);
    bindText(st, 3, kind);
    bindText(st, 4, target_type);
    bindText(st, 5, target_id);
    bindText(st, 6, plan_json);
    bindText(st, 7, plan_hash);
    sqlite3_bind_int(st, 8, risk);
    bindText(st, 9, idem_key);
    bindText(st, 10, need_approval ? "pending_approval" : "planned");
    sqlite3_bind_int64(st, 11, now);
    sqlite3_bind_int64(st, 12, now);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!ok) return false;
    out_id = (long long)sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::opsGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kOpCols + " FROM operations WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        json_out = opJson(st).dump();
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::opsFindByIdem(const std::string& actor, const std::string& idem_key, std::string& json_out) {
    if (idem_key.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kOpCols + " FROM operations WHERE actor=? AND idem_key=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    bindText(st, 1, actor);
    bindText(st, 2, idem_key);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        json_out = opJson(st).dump();
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::opsList(const std::string& devid, const std::string& status, int limit,
                      std::string& json_out) {
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kOpCols + " FROM operations";
    std::string where;
    if (!devid.empty()) where += " AND target_id=?";
    if (!status.empty()) where += " AND status=?";
    if (!where.empty()) sql += " WHERE" + where.substr(4);
    sql += " ORDER BY id DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (!devid.empty()) bindText(st, idx++, devid);
    if (!status.empty()) bindText(st, idx++, status);
    sqlite3_bind_int(st, idx, limit);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) arr.push_back(opJson(st));
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    json_out = out.dump();
    return true;
}

bool AdminDb::opsSetStatus(long long id, const std::string& status, const std::string& result,
                           const std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE operations SET status=?,result=?,error=?,updated_ms=? WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    bindText(st, 1, status);
    bindText(st, 2, result);
    bindText(st, 3, error);
    sqlite3_bind_int64(st, 4, nowMs());
    sqlite3_bind_int64(st, 5, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::opsSetApproval(long long id, const std::string& status, const std::string& approver,
                             const std::string& note, std::string& err) {
    if (status != "approved" && status != "rejected") {
        err = "bad status";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE operations SET status=?,approver=?,approval_note=?,approved_ms=?,updated_ms=? "
            "WHERE id=? AND status='pending_approval'",
            -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    long long now = nowMs();
    bindText(st, 1, status);
    bindText(st, 2, approver);
    bindText(st, 3, note);
    sqlite3_bind_int64(st, 4, now);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, id);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    int changed = done ? sqlite3_changes(db_) : 0;
    if (!done) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!done) return false;
    if (changed == 0) {
        err = "作业不在待审批状态";
        return false;
    }
    return true;
}

long long AdminDb::opsCountRecent(const std::string& actor, const std::string& devid, long long since_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql =
        "SELECT COUNT(*) FROM operations WHERE updated_ms>=? AND status IN "
        "('running','applied','verified','failed')";
    if (!actor.empty()) sql += " AND actor=?";
    if (!devid.empty()) sql += " AND target_id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return -1;
    int idx = 1;
    sqlite3_bind_int64(st, idx++, since_ms);
    if (!actor.empty()) bindText(st, idx++, actor);
    if (!devid.empty()) bindText(st, idx++, devid);
    long long n = -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = (long long)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}
