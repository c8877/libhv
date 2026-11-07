// 分发任务与文件哈希元数据
#include "admin_db.hpp"
#include "cpputil/json.hpp"

#include "sqlite3.h"

#include <string>

using nlohmann::json;

static void fillDistRow(sqlite3_stmt* st, nlohmann::json& j) {
    j["id"] = sqlite3_column_int64(st, 0);
    j["batch_id"] = (const char*)sqlite3_column_text(st, 1);
    j["devid"] = (const char*)sqlite3_column_text(st, 2);
    j["file_name"] = (const char*)sqlite3_column_text(st, 3);
    j["file_size"] = sqlite3_column_int64(st, 4);
    j["sub_dir"] = (const char*)sqlite3_column_text(st, 5);
    j["target_path"] = (const char*)sqlite3_column_text(st, 6);
    j["status"] = (const char*)sqlite3_column_text(st, 7);
    j["start_at_ms"] = sqlite3_column_int64(st, 8);
    j["deadline_at_ms"] = sqlite3_column_int64(st, 9);
    j["retry_count"] = sqlite3_column_int(st, 10);
    j["max_retries"] = sqlite3_column_int(st, 11);
    j["next_retry_ms"] = sqlite3_column_int64(st, 12);
    j["extend_count"] = sqlite3_column_int(st, 13);
    j["sent"] = sqlite3_column_int64(st, 14);
    j["error"] = (const char*)sqlite3_column_text(st, 15);
    j["relay_tid"] = sqlite3_column_int64(st, 16);
    j["actor"] = (const char*)sqlite3_column_text(st, 17);
    j["created_ms"] = sqlite3_column_int64(st, 18);
    j["started_ms"] = sqlite3_column_int64(st, 19);
    j["finished_ms"] = sqlite3_column_int64(st, 20);
    j["file_sha"] = (const char*)sqlite3_column_text(st, 21);
}

bool AdminDb::distInsert(const std::string& batch, const std::string& devid, const std::string& file,
                         long long size, const std::string& sha, const std::string& sub,
                         const std::string& target, const std::string& status,
                         long long start_ms, long long deadline_ms, int max_retries,
                         const std::string& actor, long long* id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO distribution_tasks(batch_id,devid,file_name,file_size,file_sha,"
        "sub_dir,target_path,status,start_at_ms,deadline_at_ms,max_retries,actor,created_ms)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, size);
    sqlite3_bind_text(st, 5, sha.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, sub.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, target.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, start_ms);
    sqlite3_bind_int64(st, 10, deadline_ms);
    sqlite3_bind_int(st, 11, max_retries);
    sqlite3_bind_text(st, 12, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 13, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    if (id) *id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::distUpdate(long long id, const std::string& status, const std::string& error,
                         long long sent, long long relay_tid, long long finished_ms,
                         long long next_retry_ms, int retry_count) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status=?, error=?, sent=?, relay_tid=?,"
        " finished_ms=?, next_retry_ms=?, retry_count=? WHERE id=?";
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

bool AdminDb::distProgress(long long id, long long sent) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE distribution_tasks SET sent=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, sent);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distExtend(long long id, long long deadline_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status='queued', deadline_at_ms=?,"
        " extend_count=extend_count+1, next_retry_ms=0 WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, deadline_ms);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id,batch_id,devid,file_name,file_size,sub_dir,target_path,status,"
        "start_at_ms,deadline_at_ms,retry_count,max_retries,next_retry_ms,extend_count,sent,error,"
        "relay_tid,actor,created_ms,started_ms,finished_ms,file_sha FROM distribution_tasks WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillDistRow(st, j);
        json_out = j.dump();
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::distList(const std::string& status, const std::string& q, const std::string& batch,
                       int page, int per_page, std::string& json_out, int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string where = " WHERE 1=1";
    std::vector<std::string> args;
    if (status == "active") {
        where += " AND status IN ('scheduled','queued','running')";
    } else if (status == "done") {
        where += " AND status='success'";
    } else if (status == "failed_group") {
        where += " AND status IN ('failed','timeout')";
    } else if (!status.empty()) {
        where += " AND status=?";
        args.push_back(status);
    }
    if (!batch.empty()) {
        where += " AND batch_id=?";
        args.push_back(batch);
    }
    if (!q.empty()) {
        where += " AND (devid LIKE ? OR file_name LIKE ?)";
        args.push_back("%" + q + "%");
        args.push_back("%" + q + "%");
    }
    std::string countSql = "SELECT COUNT(*) FROM distribution_tasks" + where;
    sqlite3_stmt* st = nullptr;
    total = 0;
    if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, (int)i + 1, args[i].c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    std::string sql = "SELECT id,batch_id,devid,file_name,file_size,sub_dir,target_path,status,"
        "start_at_ms,deadline_at_ms,retry_count,max_retries,next_retry_ms,extend_count,sent,error,"
        "relay_tid,actor,created_ms,started_ms,finished_ms,file_sha FROM distribution_tasks" + where +
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
        fillDistRow(st, j);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::distDue(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id,batch_id,devid,file_name,file_size,sub_dir,target_path,status,"
        "start_at_ms,deadline_at_ms,retry_count,max_retries,next_retry_ms,extend_count,sent,error,"
        "relay_tid,actor,created_ms,started_ms,finished_ms,file_sha FROM distribution_tasks"
        " WHERE status IN ('scheduled','queued','running') ORDER BY id";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    nlohmann::json arr = nlohmann::json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillDistRow(st, j);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::distSummary(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT status,COUNT(*) FROM distribution_tasks GROUP BY status",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    long long all = 0, active = 0, done = 0, failed = 0, conflict = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string s = (const char*)sqlite3_column_text(st, 0);
        long long n = sqlite3_column_int64(st, 1);
        all += n;
        if (s == "success") done += n;
        else if (s == "failed" || s == "timeout") failed += n;
        else if (s == "conflict") conflict += n;
        else if (s == "canceled") { /* 归入全部，不计进行中 */ }
        else active += n;
    }
    sqlite3_finalize(st);
    nlohmann::json j;
    j["all"] = all;
    j["active"] = active;
    j["done"] = done;
    j["failed"] = failed;
    j["conflict"] = conflict;
    json_out = j.dump();
    return true;
}

bool AdminDb::fileMetaGet(const std::string& name, long long size, long long mtime_ms,
                          std::string& sha) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT size,mtime_ms,sha256 FROM file_meta WHERE name=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_int64(st, 0) == size && sqlite3_column_int64(st, 1) == mtime_ms) {
        sha = (const char*)sqlite3_column_text(st, 2);
        ok = !sha.empty();
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::fileMetaPut(const std::string& name, long long size, long long mtime_ms,
                          const std::string& sha) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO file_meta(name,size,mtime_ms,sha256) VALUES(?,?,?,?) "
                      "ON CONFLICT(name) DO UPDATE SET size=excluded.size,"
                      " mtime_ms=excluded.mtime_ms, sha256=excluded.sha256";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, size);
    sqlite3_bind_int64(st, 3, mtime_ms);
    sqlite3_bind_text(st, 4, sha.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distAlreadyPushed(const std::string& devid, const std::string& sha) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT 1 FROM distribution_tasks WHERE devid=? AND file_sha=?"
                      " AND status IN ('success','conflict') LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, sha.c_str(), -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

