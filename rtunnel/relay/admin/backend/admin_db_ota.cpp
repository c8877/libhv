// agent OTA 升级任务
#include "admin_db.hpp"
#include "cpputil/json.hpp"

#include "sqlite3.h"

#include <string>

using nlohmann::json;

static const char* kOtaCols =
    "id,batch_id,devid,version,file_name,file_size,file_sha,force,status,sent,relay_tid,error,"
    "retry_count,max_retries,next_retry_ms,deadline_at_ms,started_ms,finished_ms,actor,created_ms";

static void fillOtaRow(sqlite3_stmt* st, nlohmann::json& j) {
    j["id"] = sqlite3_column_int64(st, 0);
    j["batch_id"] = (const char*)sqlite3_column_text(st, 1);
    j["devid"] = (const char*)sqlite3_column_text(st, 2);
    j["version"] = (const char*)sqlite3_column_text(st, 3);
    j["file_name"] = (const char*)sqlite3_column_text(st, 4);
    j["file_size"] = sqlite3_column_int64(st, 5);
    j["file_sha"] = (const char*)sqlite3_column_text(st, 6);
    j["force"] = sqlite3_column_int(st, 7);
    j["status"] = (const char*)sqlite3_column_text(st, 8);
    j["sent"] = sqlite3_column_int64(st, 9);
    j["relay_tid"] = sqlite3_column_int64(st, 10);
    j["error"] = (const char*)sqlite3_column_text(st, 11);
    j["retry_count"] = sqlite3_column_int(st, 12);
    j["max_retries"] = sqlite3_column_int(st, 13);
    j["next_retry_ms"] = sqlite3_column_int64(st, 14);
    j["deadline_at_ms"] = sqlite3_column_int64(st, 15);
    j["started_ms"] = sqlite3_column_int64(st, 16);
    j["finished_ms"] = sqlite3_column_int64(st, 17);
    j["actor"] = (const char*)sqlite3_column_text(st, 18);
    j["created_ms"] = sqlite3_column_int64(st, 19);
}

bool AdminDb::otaInsert(const std::string& batch, const std::string& devid, const std::string& version,
                        const std::string& file, long long size, const std::string& sha,
                        const std::string& status, long long deadline_ms, int max_retries,
                        const std::string& actor, int force, long long* id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO ota_tasks(batch_id,devid,version,file_name,file_size,file_sha,"
        "force,status,deadline_at_ms,max_retries,actor,created_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, size);
    sqlite3_bind_text(st, 6, sha.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, force ? 1 : 0);
    sqlite3_bind_text(st, 8, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, deadline_ms);
    sqlite3_bind_int(st, 10, max_retries);
    sqlite3_bind_text(st, 11, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 12, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    if (id) *id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::otaUpdate(long long id, const std::string& status, const std::string& error,
                        long long sent, long long relay_tid, long long finished_ms,
                        long long next_retry_ms, int retry_count) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE ota_tasks SET status=?, error=?, sent=?, relay_tid=?, finished_ms=?,"
        " next_retry_ms=?, retry_count=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, sent);
    sqlite3_bind_int64(st, 4, relay_tid);
    sqlite3_bind_int64(st, 5, finished_ms);
    sqlite3_bind_int64(st, 6, next_retry_ms);
    sqlite3_bind_int(st, 7, retry_count);
    sqlite3_bind_int64(st, 8, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::otaProgress(long long id, long long sent) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE ota_tasks SET sent=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, sent);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::otaMarkApplying(long long id, long long sent, long long relay_tid, long long now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE ota_tasks SET status='applying', sent=?, relay_tid=?, started_ms=?,"
        " next_retry_ms=0 WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, sent);
    sqlite3_bind_int64(st, 2, relay_tid);
    sqlite3_bind_int64(st, 3, now_ms);
    sqlite3_bind_int64(st, 4, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::otaGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kOtaCols + " FROM ota_tasks WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillOtaRow(st, j);
        json_out = j.dump();
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::otaList(const std::string& status, const std::string& q, const std::string& batch,
                      int page, int per_page, std::string& json_out, int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string where = " WHERE 1=1";
    std::vector<std::string> args;
    if (status == "active") {
        where += " AND status IN ('queued','running','applying')";
    } else if (status == "done") {
        where += " AND status='success'";
    } else if (status == "failed_group") {
        where += " AND status IN ('failed','timeout','unsupported')";
    } else if (!status.empty()) {
        where += " AND status=?";
        args.push_back(status);
    }
    if (!batch.empty()) {
        where += " AND batch_id=?";
        args.push_back(batch);
    }
    if (!q.empty()) {
        where += " AND (devid LIKE ? OR version LIKE ? OR file_name LIKE ?)";
        args.push_back("%" + q + "%");
        args.push_back("%" + q + "%");
        args.push_back("%" + q + "%");
    }
    std::string countSql = "SELECT COUNT(*) FROM ota_tasks" + where;
    sqlite3_stmt* st = nullptr;
    total = 0;
    if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, (int)i + 1, args[i].c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    std::string sql = std::string("SELECT ") + kOtaCols + " FROM ota_tasks" + where +
        " ORDER BY id DESC LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, idx++, args[i].c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx++, per_page);
    sqlite3_bind_int(st, idx++, (page - 1) * per_page);
    nlohmann::json arr = nlohmann::json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillOtaRow(st, j);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::otaDue(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kOtaCols +
        " FROM ota_tasks WHERE status IN ('queued','running','applying') ORDER BY id";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    nlohmann::json arr = nlohmann::json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillOtaRow(st, j);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::otaSummary(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT status,COUNT(*) FROM ota_tasks GROUP BY status",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    long long all = 0, active = 0, done = 0, failed = 0, unsupported = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string s = (const char*)sqlite3_column_text(st, 0);
        long long n = sqlite3_column_int64(st, 1);
        all += n;
        if (s == "success") done += n;
        else if (s == "unsupported") unsupported += n;
        else if (s == "failed" || s == "timeout" || s == "canceled") failed += n;
        else active += n;
    }
    sqlite3_finalize(st);
    nlohmann::json j;
    j["all"] = all;
    j["active"] = active;
    j["done"] = done;
    j["failed"] = failed;
    j["unsupported"] = unsupported;
    json_out = j.dump();
    return true;
}

bool AdminDb::otaReset(long long id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE ota_tasks SET status='queued', error='', retry_count=0,"
        " next_retry_ms=0, started_ms=0, finished_ms=0, relay_tid=0 WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}
