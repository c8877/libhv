// 账号级 AI 模型配置：每个账号一份（控制台账号 u-<id>、接入账号 c-<id>）。
//
// 密钥本体不在这里：写 <admin.db 同目录>/ai/<subject>.key（0600），
// 本表只存指纹（前 3 + 后 4 位）和时间，避免备份库/看设置的人拿到明文。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <mutex>
#include <sqlite3.h>

using nlohmann::json;

namespace {
const char* kColumns =
    "provider,endpoint,model,key_hint,key_updated_ms,advanced,updated_ms";
}  // namespace

bool AdminDb::aiProfileGet(const std::string& subject, std::string& json_out) {
    json j = json::object();
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kColumns + " FROM ai_profiles WHERE subject=?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        j["provider"] = dbColText(st, 0);
        j["endpoint"] = dbColText(st, 1);
        j["model"] = dbColText(st, 2);
        j["key_hint"] = dbColText(st, 3);
        j["key_updated_ms"] = dbColInt(st, 4);
        std::string adv = dbColText(st, 5);
        if (!adv.empty()) {
            auto a = json::parse(adv, nullptr, false);
            if (!a.is_discarded() && a.is_object()) j["advanced"] = a;
        }
        j["updated_ms"] = dbColInt(st, 6);
        j["saved"] = true;
    } else {
        j["saved"] = false;
    }
    sqlite3_finalize(st);
    json_out = j.dump();
    return true;
}

bool AdminDb::aiProfileSave(const std::string& subject, const std::string& provider,
                            const std::string& endpoint, const std::string& model,
                            const std::string& advanced, std::string& err) {
    if (subject.empty()) {
        err = "缺少主体";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO ai_profiles(subject,provider,endpoint,model,advanced,updated_ms)"
        " VALUES(?,?,?,?,?,?)"
        " ON CONFLICT(subject) DO UPDATE SET provider=excluded.provider,"
        " endpoint=excluded.endpoint,model=excluded.model,advanced=excluded.advanced,"
        " updated_ms=excluded.updated_ms";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, endpoint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, advanced.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, AdminDb::nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::aiProfileSetKey(const std::string& subject, const std::string& hint,
                              long long updated_ms, std::string& err) {
    if (subject.empty()) {
        err = "缺少主体";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    // 没有行时先建一行空配置，密钥指纹不能丢
    const char* sql =
        "INSERT INTO ai_profiles(subject,key_hint,key_updated_ms,updated_ms) VALUES(?,?,?,?)"
        " ON CONFLICT(subject) DO UPDATE SET key_hint=excluded.key_hint,"
        " key_updated_ms=excluded.key_updated_ms,updated_ms=excluded.updated_ms";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, updated_ms);
    sqlite3_bind_int64(st, 4, AdminDb::nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}
