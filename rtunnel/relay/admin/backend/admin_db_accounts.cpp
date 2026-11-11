// 接入账号（client_accounts）与 Token（client_tokens）的存取。
// Token 只落库 SHA-256 与前缀，明文只在创建/轮换时由上层返回一次。
#include "admin_db.hpp"
#include "common/sha256.hpp"
#include "common/token_util.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <chrono>
#include <cstdio>
#include <cstring>

using nlohmann::json;

namespace {

std::string colText(sqlite3_stmt* st, int col) {
    const unsigned char* p = sqlite3_column_text(st, col);
    return p ? (const char*)p : "";
}

int64_t colInt(sqlite3_stmt* st, int col) { return sqlite3_column_int64(st, col); }

bool validName(const std::string& s, size_t maxlen) {
    if (s.empty() || s.size() > maxlen) return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

json tokenJson(sqlite3_stmt* st, bool with_secret_fields = false) {
    json t;
    t["id"] = colInt(st, 0);
    t["account_id"] = colInt(st, 1);
    t["name"] = colText(st, 2);
    t["token_prefix"] = colText(st, 3);
    t["status"] = colText(st, 4);
    t["expires_at_ms"] = colInt(st, 5);
    t["quota_bytes"] = colInt(st, 6);
    t["quota_period"] = colText(st, 7);
    t["period_start_ms"] = colInt(st, 8);
    t["used_bytes"] = colInt(st, 9);
    t["rate_per_min"] = (int)colInt(st, 10);
    t["burst"] = (int)colInt(st, 11);
    t["max_sessions"] = (int)colInt(st, 12);
    t["quota_action"] = colText(st, 13);
    t["created_by"] = colText(st, 14);
    t["created_ms"] = colInt(st, 15);
    t["last_used_ms"] = colInt(st, 16);
    t["last_used_ip"] = colText(st, 17);
    t["revoked_ms"] = colInt(st, 18);
    if (with_secret_fields) t["token_hash"] = colText(st, 19);
    return t;
}

const char* kTokenSelect =
    "SELECT id,account_id,name,token_prefix,status,expires_at_ms,quota_bytes,quota_period,"
    "period_start_ms,used_bytes,rate_per_min,burst,max_sessions,quota_action,created_by,"
    "created_ms,last_used_ms,last_used_ip,revoked_ms,token_hash FROM client_tokens";

} // namespace

bool AdminDb::clientAccountList(const std::string& q, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json arr = json::array();
    std::string sql =
        "SELECT a.id,a.username,a.display_name,a.status,a.note,a.created_by,a.created_ms,a.updated_ms,"
        "(SELECT COUNT(*) FROM client_tokens t WHERE t.account_id=a.id AND t.status!='revoked'),"
        "(SELECT COALESCE(MAX(t.last_used_ms),0) FROM client_tokens t WHERE t.account_id=a.id) "
        "FROM client_accounts a";
    if (!q.empty()) sql += " WHERE a.username LIKE ? OR a.display_name LIKE ?";
    sql += " ORDER BY a.username";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        json a;
        a["id"] = colInt(st, 0);
        a["username"] = colText(st, 1);
        a["display_name"] = colText(st, 2);
        a["status"] = colText(st, 3);
        a["note"] = colText(st, 4);
        a["created_by"] = colText(st, 5);
        a["created_ms"] = colInt(st, 6);
        a["updated_ms"] = colInt(st, 7);
        a["token_count"] = colInt(st, 8);
        a["last_used_ms"] = colInt(st, 9);
        arr.push_back(a);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::clientAccountGet(long long id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT a.id,a.username,a.display_name,a.status,a.note,a.created_by,a.created_ms,a.updated_ms,"
        "(SELECT COUNT(*) FROM client_tokens t WHERE t.account_id=a.id),"
        "(SELECT COUNT(*) FROM client_tokens t WHERE t.account_id=a.id AND t.status='active'),"
        "(SELECT COUNT(*) FROM client_tokens t WHERE t.account_id=a.id AND t.status='disabled'),"
        "(SELECT COUNT(*) FROM client_tokens t WHERE t.account_id=a.id AND t.status='revoked'),"
        "(SELECT COALESCE(MAX(t.last_used_ms),0) FROM client_tokens t WHERE t.account_id=a.id),"
        "(SELECT t.last_used_ip FROM client_tokens t WHERE t.account_id=a.id "
        " ORDER BY t.last_used_ms DESC LIMIT 1),"
        "(SELECT COUNT(*) FROM client_group_members m WHERE m.username=a.username) "
        "FROM client_accounts a WHERE a.id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "account not found";
        return false;
    }
    json a;
    a["id"] = colInt(st, 0);
    a["username"] = colText(st, 1);
    a["display_name"] = colText(st, 2);
    a["status"] = colText(st, 3);
    a["note"] = colText(st, 4);
    a["created_by"] = colText(st, 5);
    a["created_ms"] = colInt(st, 6);
    a["updated_ms"] = colInt(st, 7);
    a["token_count"] = colInt(st, 8);
    a["token_active"] = colInt(st, 9);
    a["token_disabled"] = colInt(st, 10);
    a["token_revoked"] = colInt(st, 11);
    a["last_used_ms"] = colInt(st, 12);
    a["last_used_ip"] = colText(st, 13);
    a["group_count"] = colInt(st, 14);
    sqlite3_finalize(st);
    json_out = a.dump();
    return true;
}

bool AdminDb::clientAccountCreate(const std::string& username, const std::string& display_name,
                                  const std::string& note, const std::string& by,
                                  long long& out_id, std::string& err) {
    if (!validName(username, 64)) { err = "用户名需为 1-64 字节可见字符"; return false; }
    if (display_name.size() > 128 || note.size() > 256) { err = "展示名或备注过长"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO client_accounts(username,display_name,status,note,created_by,created_ms,updated_ms) "
                      "VALUES(?,?,'active',?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    long long now = nowMs();
    sqlite3_bind_text(st, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, now);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!ok) {
        if (err.find("UNIQUE") != std::string::npos) err = "用户名已存在";
        return false;
    }
    out_id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::clientAccountUpdate(long long id, const std::string& display_name,
                                  const std::string& note, const std::string& status,
                                  std::string& err) {
    if (status != "active" && status != "disabled") { err = "bad status"; return false; }
    if (display_name.size() > 128 || note.size() > 256) { err = "展示名或备注过长"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE client_accounts SET display_name=?,note=?,status=?,updated_ms=? "
                                "WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, display_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, nowMs());
    sqlite3_bind_int64(st, 5, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "account not found";
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::clientAccountDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    std::string sid = std::to_string(id);
    std::string username;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT username FROM client_accounts WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) username = colText(st, 0);
    }
    sqlite3_finalize(st);
    if (username.empty()) { exec("ROLLBACK", e); err = "account not found"; return false; }
    bool ok = exec(("DELETE FROM client_tokens WHERE account_id=" + sid).c_str(), e);
    if (ok && !username.empty()) {
        // 分组成员按用户名引用，一并清理，避免留下悬空授权
        sqlite3_stmt* d = nullptr;
        if (sqlite3_prepare_v2(db_, "DELETE FROM client_group_members WHERE username=?", -1, &d, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(d, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(d) == SQLITE_DONE;
            sqlite3_finalize(d);
        }
    }
    ok = ok && exec(("DELETE FROM client_accounts WHERE id=" + sid).c_str(), e);
    exec(ok ? "COMMIT" : "ROLLBACK", e);
    if (!ok) err = e;
    return ok;
}

bool AdminDb::clientTokenList(long long account_id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string(kTokenSelect) + " WHERE account_id=? ORDER BY id DESC";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int64(st, 1, account_id);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) arr.push_back(tokenJson(st));
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::clientTokenCreate(long long account_id, const DbTokenSpec& spec, const std::string& by,
                                int max_per_account, std::string& plain_out, std::string& json_out,
                                std::string& err) {
    if (spec.name.size() > 64) { err = "名称过长"; return false; }
    plain_out = rtutil::newClientToken();
    if (plain_out.empty()) { err = "随机数不可用"; return false; }
    std::string hash = rtutil::sha256Hex(plain_out);
    std::string prefix = rtutil::clientTokenPrefix(plain_out);

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT status FROM client_accounts WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, account_id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "account not found"; return false; }
    sqlite3_finalize(st);

    int n = 0;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM client_tokens WHERE account_id=? AND status!='revoked'",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, account_id);
        if (sqlite3_step(st) == SQLITE_ROW) n = (int)colInt(st, 0);
    }
    sqlite3_finalize(st);
    if (max_per_account > 0 && n >= max_per_account) {
        err = "该账号 Token 数已达上限 " + std::to_string(max_per_account);
        return false;
    }

    rtutil::TokenPeriod per = rtutil::tokenPeriodFromName(spec.quota_period);
    long long period_start = rtutil::tokenPeriodStart(per, nowMs());
    const char* sql =
        "INSERT INTO client_tokens(account_id,name,token_hash,token_prefix,status,expires_at_ms,"
        "quota_bytes,quota_period,period_start_ms,used_bytes,rate_per_min,burst,max_sessions,"
        "quota_action,created_by,created_ms) VALUES(?,?,?,?,'active',?,?,?,?,0,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int64(st, 1, account_id);
    sqlite3_bind_text(st, 2, spec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, spec.expires_at_ms);
    sqlite3_bind_int64(st, 6, spec.quota_bytes);
    sqlite3_bind_text(st, 7, rtutil::tokenPeriodName(per), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, period_start);
    sqlite3_bind_int(st, 9, spec.rate_per_min);
    sqlite3_bind_int(st, 10, spec.burst);
    sqlite3_bind_int(st, 11, spec.max_sessions);
    sqlite3_bind_text(st, 12, spec.quota_action == "block" ? "block" : "cut", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 13, by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 14, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!ok) return false;
    long long id = sqlite3_last_insert_rowid(db_);

    sqlite3_stmt* q = nullptr;
    std::string qsql = std::string(kTokenSelect) + " WHERE id=?";
    if (sqlite3_prepare_v2(db_, qsql.c_str(), -1, &q, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) == SQLITE_ROW) json_out = tokenJson(q).dump();
        sqlite3_finalize(q);
    }
    return true;
}

bool AdminDb::clientTokenSpec(long long id, DbTokenSpec& spec, long long& account_id,
                              std::string* status, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT account_id,name,expires_at_ms,quota_bytes,quota_period,rate_per_min,"
                      "burst,max_sessions,quota_action,status FROM client_tokens WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "token not found"; return false; }
    account_id = colInt(st, 0);
    spec.name = colText(st, 1);
    spec.expires_at_ms = colInt(st, 2);
    spec.quota_bytes = colInt(st, 3);
    spec.quota_period = colText(st, 4);
    spec.rate_per_min = (int)colInt(st, 5);
    spec.burst = (int)colInt(st, 6);
    spec.max_sessions = (int)colInt(st, 7);
    spec.quota_action = colText(st, 8);
    if (status != nullptr) *status = colText(st, 9);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::clientTokenUpdate(long long id, const DbTokenSpec& spec, const std::string& status,
                                std::string& err) {
    if (spec.name.size() > 64) { err = "名称过长"; return false; }
    if (status != "active" && status != "disabled" && status != "revoked") {
        err = "bad status";
        return false;
    }
    rtutil::TokenPeriod per = rtutil::tokenPeriodFromName(spec.quota_period);
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    // 周期变化时重置计量窗口，避免旧周期的用量落到新周期
    const char* sql =
        "UPDATE client_tokens SET name=?,status=?,expires_at_ms=?,quota_bytes=?,quota_period=?,"
        "rate_per_min=?,burst=?,max_sessions=?,quota_action=?,"
        "period_start_ms=CASE WHEN quota_period<>? THEN ? ELSE period_start_ms END,"
        "used_bytes=CASE WHEN quota_period<>? THEN 0 ELSE used_bytes END,"
        "revoked_ms=CASE WHEN ?='revoked' AND revoked_ms=0 THEN ? ELSE revoked_ms END "
        "WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    const char* per_name = rtutil::tokenPeriodName(per);
    long long now = nowMs();
    long long period_start = rtutil::tokenPeriodStart(per, now);
    sqlite3_bind_text(st, 1, spec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, spec.expires_at_ms);
    sqlite3_bind_int64(st, 4, spec.quota_bytes);
    sqlite3_bind_text(st, 5, per_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, spec.rate_per_min);
    sqlite3_bind_int(st, 7, spec.burst);
    sqlite3_bind_int(st, 8, spec.max_sessions);
    sqlite3_bind_text(st, 9, spec.quota_action == "block" ? "block" : "cut", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, per_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 11, period_start);
    sqlite3_bind_text(st, 12, per_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 13, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 14, now);
    sqlite3_bind_int64(st, 15, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "token not found";
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::clientTokenRotate(long long id, std::string& plain_out, std::string& json_out,
                                std::string& err) {
    plain_out = rtutil::newClientToken();
    if (plain_out.empty()) { err = "随机数不可用"; return false; }
    std::string hash = rtutil::sha256Hex(plain_out);
    std::string prefix = rtutil::clientTokenPrefix(plain_out);

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string per;
    if (sqlite3_prepare_v2(db_, "SELECT quota_period FROM client_tokens WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); err = "token not found"; return false; }
    per = colText(st, 0);
    sqlite3_finalize(st);

    long long period_start = rtutil::tokenPeriodStart(rtutil::tokenPeriodFromName(per), nowMs());
    const char* sql =
        "UPDATE client_tokens SET token_hash=?,token_prefix=?,status='active',revoked_ms=0,"
        "period_start_ms=?,used_bytes=0 WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, period_start);
    sqlite3_bind_int64(st, 4, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "token not found";
    sqlite3_finalize(st);
    if (!ok) return false;

    sqlite3_stmt* q = nullptr;
    std::string qsql = std::string(kTokenSelect) + " WHERE id=?";
    if (sqlite3_prepare_v2(db_, qsql.c_str(), -1, &q, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, id);
        if (sqlite3_step(q) == SQLITE_ROW) json_out = tokenJson(q).dump();
        sqlite3_finalize(q);
    }
    return true;
}

bool AdminDb::clientTokenDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE client_tokens SET status='revoked',revoked_ms=? WHERE id=?",
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

bool AdminDb::clientAuthPolicies(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json accounts = json::object();
    sqlite3_stmt* ast = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT username,status FROM client_accounts", -1, &ast, nullptr) == SQLITE_OK) {
        while (sqlite3_step(ast) == SQLITE_ROW) accounts[colText(ast, 0)] = colText(ast, 1);
    }
    sqlite3_finalize(ast);
    const char* sql =
        "SELECT t.id,t.account_id,a.username,t.token_hash,t.token_prefix,t.name,t.status,"
        "t.expires_at_ms,t.quota_bytes,t.quota_period,t.period_start_ms,t.used_bytes,"
        "t.rate_per_min,t.burst,t.max_sessions,t.quota_action,a.status "
        "FROM client_tokens t JOIN client_accounts a ON a.id=t.account_id "
        "WHERE t.status!='revoked'";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json t;
        t["id"] = colInt(st, 0);
        t["account_id"] = colInt(st, 1);
        t["username"] = colText(st, 2);
        t["hash"] = colText(st, 3);
        t["prefix"] = colText(st, 4);
        t["name"] = colText(st, 5);
        t["status"] = colText(st, 6);
        t["expires_at_ms"] = colInt(st, 7);
        t["quota_bytes"] = colInt(st, 8);
        t["quota_period"] = colText(st, 9);
        t["period_start_ms"] = colInt(st, 10);
        t["used_bytes"] = colInt(st, 11);
        t["rate_per_min"] = (int)colInt(st, 12);
        t["burst"] = (int)colInt(st, 13);
        t["max_sessions"] = (int)colInt(st, 14);
        t["quota_action"] = colText(st, 15);
        t["account_status"] = colText(st, 16);
        arr.push_back(t);
    }
    sqlite3_finalize(st);
    json out;
    out["accounts"] = accounts;
    out["tokens"] = arr;
    json_out = out.dump();
    return true;
}

bool AdminDb::clientTokenUsageApply(const std::string& json_in, std::string& err) {
    json arr = json::parse(json_in, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) { err = "bad snapshot"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE client_tokens SET used_bytes=?,last_used_ms=?,last_used_ip=? "
        "WHERE id=? AND period_start_ms=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    for (auto& item : arr) {
        if (!item.is_object()) continue;
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_int64(st, 1, item.value("used_bytes", 0LL));
        sqlite3_bind_int64(st, 2, item.value("last_used_ms", 0LL));
        sqlite3_bind_text(st, 3, item.value("last_used_ip", std::string()).c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, item.value("id", 0LL));
        sqlite3_bind_int64(st, 5, item.value("period_start_ms", 0LL));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    return true;
}
