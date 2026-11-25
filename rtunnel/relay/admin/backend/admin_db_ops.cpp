// 操作作业（operations）：AI 与人共用的可重放写动作。
//
// 三阶段：
//   plan   —— 只做校验与摘要，落一行 status=planned，plan_hash 冻结“将要做什么”
//   apply  —— 必须带同一个 plan_hash 才执行；带 Idempotency-Key 时 (actor,key) 唯一
//   verify —— 复核执行结果（P0 阶段把 apply 的结果记下来，探针由调用方另行提交）
//
// 目前只有 kind=exec；后续 svc/ota 复用同一张表与同一套状态。
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
    std::string res = dbColText(st, 11);
    if (!res.empty()) {
        json r = json::parse(res, nullptr, false);
        j["result"] = r.is_discarded() ? json(res) : r;
    }
    std::string err = dbColText(st, 12);
    if (!err.empty()) j["error"] = err;
    j["created_ms"] = (long long)sqlite3_column_int64(st, 13);
    j["updated_ms"] = (long long)sqlite3_column_int64(st, 14);
    return j;
}

const char* kOpCols =
    "id,actor,actor_type,kind,target_type,target_id,plan,plan_hash,risk,idem_key,status,result,"
    "error,created_ms,updated_ms";

}  // namespace

bool AdminDb::opsCreate(const std::string& actor, const std::string& actor_type, const std::string& kind,
                        const std::string& target_type, const std::string& target_id,
                        const std::string& plan_json, const std::string& plan_hash, int risk,
                        const std::string& idem_key, long long& out_id, std::string& err) {
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
            "idem_key,status,created_ms,updated_ms) VALUES(?,?,?,?,?,?,?,?,?,'planned',?,?)",
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
    sqlite3_bind_int64(st, 10, now);
    sqlite3_bind_int64(st, 11, now);
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

bool AdminDb::opsList(const std::string& devid, int limit, std::string& json_out) {
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT ") + kOpCols + " FROM operations";
    if (!devid.empty()) sql += " WHERE target_id=?";
    sql += " ORDER BY id DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (!devid.empty()) bindText(st, idx++, devid);
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
