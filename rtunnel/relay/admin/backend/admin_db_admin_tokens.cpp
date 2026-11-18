// 管理台账号 API Token（admin_tokens）的存取。
// 与接入账号 Token 一致：只落库 SHA-256 与前缀，明文仅在创建/轮换时返回一次。
// 资源权限组：perm_group_members(subject_type='atk')，subject_id 为 token id 的十进制字符串。
#include "admin_db.hpp"
#include "common/sha256.hpp"
#include "common/token_util.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

using nlohmann::json;

namespace {

json tokenJson(sqlite3_stmt* st) {
    json t;
    t["id"] = dbColInt(st, 0);
    t["user_id"] = dbColInt(st, 1);
    t["username"] = dbColText(st, 2);
    t["name"] = dbColText(st, 3);
    t["token_prefix"] = dbColText(st, 4);
    t["status"] = dbColText(st, 5);
    t["expires_at_ms"] = dbColInt(st, 6);
    t["created_by"] = dbColText(st, 7);
    t["created_ms"] = dbColInt(st, 8);
    t["last_used_ms"] = dbColInt(st, 9);
    t["last_used_ip"] = dbColText(st, 10);
    t["revoked_ms"] = dbColInt(st, 11);
    t["group_count"] = dbColInt(st, 12);
    t["groups"] = dbColText(st, 13);
    t["role"] = dbColText(st, 14);
    return t;
}

const char* kTokenSelect =
    "SELECT t.id,t.user_id,t.username,t.name,t.token_prefix,t.status,t.expires_at_ms,"
    "t.created_by,t.created_ms,t.last_used_ms,t.last_used_ip,t.revoked_ms,"
    "(SELECT COUNT(*) FROM perm_group_members m WHERE m.subject_type='atk' "
    " AND m.subject_id=CAST(t.id AS TEXT)),"
    "(SELECT GROUP_CONCAT(g.name, ', ') FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id "
    " WHERE m.subject_type='atk' AND m.subject_id=CAST(t.id AS TEXT)),"
    "COALESCE(u.role,'') "
    "FROM admin_tokens t LEFT JOIN users u ON u.id=t.user_id";

// 读取 token 归属，用于权限判断
bool tokenOwner(sqlite3* db, long long id, long long& user_id, std::string& username,
                std::string& status, std::string& err) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT user_id,username,status FROM admin_tokens WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "token not found";
        return false;
    }
    user_id = dbColInt(st, 0);
    username = dbColText(st, 1);
    status = dbColText(st, 2);
    sqlite3_finalize(st);
    return true;
}

} // namespace

bool AdminDb::adminTokenList(long long user_id, const std::string& q, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = kTokenSelect;
    bool where = false;
    if (user_id > 0) {
        sql += " WHERE t.user_id=?";
        where = true;
    }
    if (!q.empty()) {
        sql += where ? " AND" : " WHERE";
        sql += " (t.name LIKE ? OR t.username LIKE ? OR t.token_prefix LIKE ?)";
    }
    sql += " ORDER BY t.id DESC";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (user_id > 0) sqlite3_bind_int64(st, idx++, user_id);
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) arr.push_back(tokenJson(st));
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::adminTokenCreate(long long user_id, const std::string& username,
                               const DbTokenSpec& spec, const std::string& by, int max_per_user,
                               std::string& plain_out, std::string& json_out, std::string& err) {
    if (spec.name.size() > 64) { err = "名称过长"; return false; }
    std::lock_guard<std::mutex> lk(mu_);

    // 归属账号必须存在且启用；username 以库内为准
    std::string uname, ustatus;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT username,status FROM users WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, user_id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "账号不存在"; return false; }
    uname = dbColText(st, 0);
    ustatus = dbColText(st, 1);
    sqlite3_finalize(st);
    if (ustatus != "active") { err = "账号已停用"; return false; }
    if (!username.empty() && username != uname) { err = "账号不匹配"; return false; }

    int n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM admin_tokens WHERE user_id=? AND status!='revoked'",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, user_id);
        if (sqlite3_step(st) == SQLITE_ROW) n = (int)dbColInt(st, 0);
    }
    sqlite3_finalize(st);
    if (max_per_user > 0 && n >= max_per_user) {
        err = "该账号 Token 数已达上限 " + std::to_string(max_per_user);
        return false;
    }

    plain_out = rtutil::newAdminToken();
    if (plain_out.empty()) { err = "随机数不可用"; return false; }
    std::string hash = rtutil::sha256Hex(plain_out);
    std::string prefix = rtutil::tokenPrefix(plain_out);

    const char* sql =
        "INSERT INTO admin_tokens(user_id,username,name,token_hash,token_prefix,status,"
        "expires_at_ms,created_by,created_ms) VALUES(?,?,?,?,?,'active',?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_bind_text(st, 2, uname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, spec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, spec.expires_at_ms);
    sqlite3_bind_text(st, 7, by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!ok) return false;
    long long id = sqlite3_last_insert_rowid(db_);

    sqlite3_stmt* q = nullptr;
    std::string qsql = std::string(kTokenSelect) + " WHERE t.id=?";
    if (sqlite3_prepare_v2(db_, qsql.c_str(), -1, &q, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) == SQLITE_ROW) json_out = tokenJson(q).dump();
        sqlite3_finalize(q);
    }
    return true;
}

bool AdminDb::adminTokenSpec(long long id, DbTokenSpec& spec, long long& user_id, std::string& username,
                             std::string* status, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT user_id,username,name,expires_at_ms,status FROM admin_tokens WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "token not found"; return false; }
    user_id = dbColInt(st, 0);
    username = dbColText(st, 1);
    spec.name = dbColText(st, 2);
    spec.expires_at_ms = dbColInt(st, 3);
    if (status != nullptr) *status = dbColText(st, 4);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::adminTokenUpdate(long long id, const DbTokenSpec& spec, const std::string& status,
                               std::string& err) {
    if (spec.name.size() > 64) { err = "名称过长"; return false; }
    if (status != "active" && status != "disabled" && status != "revoked") {
        err = "bad status";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE admin_tokens SET name=?,status=?,expires_at_ms=?,"
        "revoked_ms=CASE WHEN ?='revoked' AND revoked_ms=0 THEN ? ELSE revoked_ms END "
        "WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    long long now = nowMs();
    sqlite3_bind_text(st, 1, spec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, spec.expires_at_ms);
    sqlite3_bind_text(st, 4, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "token not found";
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::adminTokenRotate(long long id, std::string& plain_out, std::string& json_out,
                               std::string& err) {
    plain_out = rtutil::newAdminToken();
    if (plain_out.empty()) { err = "随机数不可用"; return false; }
    std::string hash = rtutil::sha256Hex(plain_out);
    std::string prefix = rtutil::tokenPrefix(plain_out);

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE admin_tokens SET token_hash=?,token_prefix=?,status='active',revoked_ms=0,"
        "last_used_ms=0,last_used_ip='' WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "token not found";
    sqlite3_finalize(st);
    if (!ok) return false;

    sqlite3_stmt* q = nullptr;
    std::string qsql = std::string(kTokenSelect) + " WHERE t.id=?";
    if (sqlite3_prepare_v2(db_, qsql.c_str(), -1, &q, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) == SQLITE_ROW) json_out = tokenJson(q).dump();
        sqlite3_finalize(q);
    }
    return true;
}

bool AdminDb::adminTokenDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE admin_tokens SET status='revoked',revoked_ms=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, nowMs());
    sqlite3_bind_int64(st, 2, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "token not found";
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::adminTokenAuth(const std::string& hash, long long now_ms, const std::string& ip,
                             DbUser& user, long long& token_id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT t.id,t.status,t.expires_at_ms,t.revoked_ms,t.last_used_ms,"
        "u.id,u.username,u.role,u.status,u.created_ms "
        "FROM admin_tokens t JOIN users u ON u.id=t.user_id WHERE t.token_hash=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "token not found";
        return false;
    }
    long long id = dbColInt(st, 0);
    std::string tstatus = dbColText(st, 1);
    long long expires = dbColInt(st, 2);
    long long revoked = dbColInt(st, 3);
    long long last_used = dbColInt(st, 4);
    user.id = dbColInt(st, 5);
    user.username = dbColText(st, 6);
    user.role = dbColText(st, 7);
    user.status = dbColText(st, 8);
    user.created_ms = dbColInt(st, 9);
    sqlite3_finalize(st);

    if (tstatus != "active" || revoked != 0) { err = "token disabled"; return false; }
    if (expires > 0 && now_ms >= expires) { err = "token expired"; return false; }
    if (user.status != "active") { err = "account disabled"; return false; }

    // 30 秒内的重复请求不再写库，避免每个 API 调用都产生一次 WAL 写入
    if (now_ms - last_used > 30000) {
        sqlite3_stmt* u = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE admin_tokens SET last_used_ms=?,last_used_ip=? WHERE id=?",
                               -1, &u, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(u, 1, now_ms);
            sqlite3_bind_text(u, 2, ip.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(u, 3, id);
            sqlite3_step(u);
            sqlite3_finalize(u);
        }
    }
    token_id = id;
    return true;
}

bool AdminDb::adminTokenGroups(long long token_id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT g.id,g.name,g.description,"
        "(SELECT COUNT(*) FROM dev_group_members m WHERE m.group_id=g.id),"
        "EXISTS(SELECT 1 FROM subject_groups s WHERE s.group_id=g.id AND s.subject_type='atk' "
        " AND s.subject_id=?) "
        "FROM dev_groups g ORDER BY g.name";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    std::string sid = std::to_string(token_id);
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    json groups = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json g;
        g["id"] = dbColInt(st, 0);
        g["name"] = dbColText(st, 1);
        g["description"] = dbColText(st, 2);
        g["device_count"] = dbColInt(st, 3);
        g["selected"] = sqlite3_column_int(st, 4) != 0;
        groups.push_back(g);
    }
    sqlite3_finalize(st);
    json out;
    out["groups"] = groups;
    json_out = out.dump();
    return true;
}

bool AdminDb::adminTokenSetGroups(long long token_id, const std::vector<long long>& ids,
                                  std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    long long owner = 0;
    std::string uname, status;
    if (!tokenOwner(db_, token_id, owner, uname, status, err)) return false;

    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    std::string sid = std::to_string(token_id);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM subject_groups WHERE subject_type='atk' AND subject_id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) { exec("ROLLBACK", e); err = "delete failed"; return false; }

    for (long long id : ids) {
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM dev_groups WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = "db error";
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        bool exists = sqlite3_step(st) == SQLITE_ROW;
        sqlite3_finalize(st);
        if (!exists) { exec("ROLLBACK", e); err = "group not found"; return false; }
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO subject_groups(subject_type,subject_id,group_id,added_ms) "
                                   "VALUES('atk',?,?,?)", -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = "db error";
            return false;
        }
        sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, id);
        sqlite3_bind_int64(st, 3, nowMs());
        ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) { exec("ROLLBACK", e); err = "insert failed"; return false; }
    }
    if (!exec("COMMIT", e)) { exec("ROLLBACK", e); err = e; return false; }
    return true;
}
