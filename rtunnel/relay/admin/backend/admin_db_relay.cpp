// relay 地址字典、有序下发列表、同步任务（任务级 meta）。
// 逐设备执行行复用 distribution_tasks（kind='relay'），这里只维护任务级状态。
#include "admin_db.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <string>
#include <vector>

using nlohmann::json;

namespace {

int itemCountLocked(sqlite3* db, const char* batch) {
    sqlite3_stmt* st = nullptr;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM distribution_tasks WHERE batch_id=? AND kind='relay'",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, batch, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

// 单条件计数：一张表上的一批状态
int countByStatus(sqlite3* db, const char* batch, const char* statuses, int wave = -1) {
    sqlite3_stmt* st = nullptr;
    std::string sql = "SELECT COUNT(*) FROM distribution_tasks WHERE batch_id=? AND kind='relay'"
                      " AND status IN (" + std::string(statuses) + ")";
    if (wave > 0) sql += " AND wave=?";
    int n = 0;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, batch, -1, SQLITE_TRANSIENT);
        if (wave > 0) sqlite3_bind_int(st, 2, wave);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

} // namespace

// relay_dict 存的是裸 host（IPv6 不带方括号）。逗号/空白会被拼接成多个端点，
// 直接拒绝，避免一条字典项在下发列表里变成两条地址。
static bool validRelayHost(const std::string& h, std::string& err) {
    if (h.empty() || h.size() > 255) { err = "地址不能为空且不超过 255 字符"; return false; }
    for (unsigned char c : h) {
        if (c <= 0x20 || c == 0x7f || c == ',' || c == '/' || c == '[' || c == ']') {
            err = "地址包含非法字符";
            return false;
        }
    }
    return true;
}

bool AdminDb::relayDictPage(const std::string& q, int page, int page_size, std::string& json_out,
                            int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string where;
    if (!q.empty()) where = " WHERE (d.host LIKE ? OR d.name LIKE ? OR d.description LIKE ?)";
    std::string kw = "%" + q + "%";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, ("SELECT COUNT(*) FROM relay_dict d" + where).c_str(), -1, &st, nullptr) != SQLITE_OK)
        return false;
    if (!q.empty()) {
        sqlite3_bind_text(st, 1, kw.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, kw.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, kw.c_str(), -1, SQLITE_TRANSIENT);
    }
    total = 0;
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    if (page < 1) page = 1;
    if (page_size < 1 || page_size > 200) page_size = 50;
    const char* sql =
        "SELECT d.id,d.host,d.port,d.name,d.description,d.enabled,"
        " (SELECT l.position FROM relay_list l WHERE l.endpoint_id=d.id) AS pos"
        " FROM relay_dict d";
    std::string full = sql + where + " ORDER BY (pos IS NULL), pos, d.id LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db_, full.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (!q.empty()) {
        sqlite3_bind_text(st, idx++, kw.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, kw.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, kw.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, idx++, page_size);
    sqlite3_bind_int(st, idx++, (page - 1) * page_size);

    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = (long long)sqlite3_column_int64(st, 0);
        j["host"] = dbColText(st, 1);
        j["port"] = sqlite3_column_int(st, 2);
        j["name"] = dbColText(st, 3);
        j["description"] = dbColText(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        if (sqlite3_column_type(st, 6) == SQLITE_NULL) {
            j["in_list"] = false;
        } else {
            j["in_list"] = true;
            j["position"] = sqlite3_column_int(st, 6) + 1;
        }
        arr.push_back(j);
    }
    sqlite3_finalize(st);

    json out;
    out["items"] = arr;
    out["total"] = total;
    out["page"] = page;
    out["page_size"] = page_size;
    json_out = out.dump();
    return true;
}

bool AdminDb::relayDictAdd(const std::string& host, int port, const std::string& name,
                           const std::string& desc, long long& out_id, std::string& err) {
    if (!validRelayHost(host, err) || port <= 0 || port > 65535) {
        if (err.empty()) err = "端口不合法";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    long long now = nowMs();
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO relay_dict(host,port,name,description,enabled,created_ms,updated_ms)"
                      " VALUES(?,?,?,?,1,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, port);
    sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, now);
    sqlite3_bind_int64(st, 6, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_CONSTRAINT) { err = "该地址已存在"; return false; }
    if (rc != SQLITE_DONE) { err = "db error"; return false; }
    out_id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::relayDictUpdate(long long id, const std::string& host, int port,
                              const std::string& name, const std::string& desc, int enabled,
                              std::string& err) {
    if (!validRelayHost(host, err) || port <= 0 || port > 65535) {
        if (err.empty()) err = "端口不合法";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled) {
        sqlite3_stmt* ck = nullptr;
        bool used = false;
        if (sqlite3_prepare_v2(db_, "SELECT 1 FROM relay_list WHERE endpoint_id=?", -1, &ck, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(ck, 1, id);
            used = sqlite3_step(ck) == SQLITE_ROW;
        }
        sqlite3_finalize(ck);
        if (used) { err = "该地址在下发列表中，请先移出再停用"; return false; }
    }
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE relay_dict SET host=?,port=?,name=?,description=?,enabled=?,updated_ms=?"
                      " WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, port);
    sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 6, nowMs());
    sqlite3_bind_int64(st, 7, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_CONSTRAINT) { err = "该地址已存在"; return false; }
    if (rc != SQLITE_DONE) { err = "db error"; return false; }
    return true;
}

bool AdminDb::relayDictDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* ck = nullptr;
    bool used = false;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM relay_list WHERE endpoint_id=?", -1, &ck, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(ck, 1, id);
        used = sqlite3_step(ck) == SQLITE_ROW;
    }
    sqlite3_finalize(ck);
    if (used) { err = "该地址在下发列表中，请先移出"; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM relay_dict WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = "db error"; return false; }
    return true;
}

bool AdminDb::relayListJson(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT l.position,d.id,d.host,d.port,d.name,d.enabled"
        " FROM relay_list l JOIN relay_dict d ON d.id=l.endpoint_id"
        " ORDER BY l.position";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["position"] = sqlite3_column_int(st, 0) + 1;
        j["endpoint_id"] = (long long)sqlite3_column_int64(st, 1);
        j["host"] = dbColText(st, 2);
        j["port"] = sqlite3_column_int(st, 3);
        j["name"] = dbColText(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::relayListSet(const std::vector<long long>& ids, const std::string& by, std::string& err) {
    if (ids.size() > 8) { err = "下发列表最多 8 个地址"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    std::set<long long> uniq;
    for (long long id : ids) {
        if (!uniq.insert(id).second) { err = "地址重复"; return false; }
    }
    // 校验地址存在且启用，顺便拿规范化 host:port 由调用方生成
    for (long long id : ids) {
        sqlite3_stmt* st = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(db_, "SELECT enabled FROM relay_dict WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, id);
            if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0) != 0;
        }
        sqlite3_finalize(st);
        if (!ok) { err = "地址不存在或已停用"; return false; }
    }
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    if (!exec("DELETE FROM relay_list", err)) { exec("ROLLBACK", err); return false; }
    long long now = nowMs();
    for (size_t i = 0; i < ids.size(); ++i) {
        sqlite3_stmt* st = nullptr;
        const char* sql = "INSERT INTO relay_list(position,endpoint_id,added_by,added_ms) VALUES(?,?,?,?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", err);
            err = "db error";
            return false;
        }
        sqlite3_bind_int(st, 1, (int)i);
        sqlite3_bind_int64(st, 2, ids[i]);
        sqlite3_bind_text(st, 3, by.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, now);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            exec("ROLLBACK", err);
            err = "db error";
            return false;
        }
    }
    std::string e;
    if (!exec("COMMIT", e)) { exec("ROLLBACK", e); err = "db error"; return false; }
    return true;
}

bool AdminDb::relayTaskCreate(const std::string& batch, const std::string& name,
                              const std::string& list_json, long long list_rev, int allow_leave,
                              const std::string& mode, int wave_size, int wave_interval_s,
                              int fail_threshold_pct, const std::vector<std::string>& devids,
                              long long start_ms, long long deadline_ms, int max_retries,
                              const std::string& actor, std::string& err) {
    if (devids.empty()) { err = "没有目标设备"; return false; }
    int n = (int)devids.size();
    bool wave = (mode == "wave");
    if (!wave) wave_size = n;
    if (wave_size < 1) wave_size = 1;
    if (wave_size > n) wave_size = n;
    int total_waves = (n + wave_size - 1) / wave_size;
    if (wave_interval_s < 0) wave_interval_s = 0;
    if (fail_threshold_pct < 0) fail_threshold_pct = 0;
    if (fail_threshold_pct > 100) fail_threshold_pct = 100;

    std::lock_guard<std::mutex> lk(mu_);
    long long now = nowMs();
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    {
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO relay_sync_task(batch_id,name,list_json,list_rev,allow_leave,mode,"
            " wave_size,wave_interval_s,fail_threshold_pct,total_waves,current_wave,wave_done_ms,"
            " status,start_at_ms,deadline_at_ms,max_retries,actor,created_ms,finished_ms)"
            " VALUES(?,?,?,?,?,?,?,?,?,?,0,0,'scheduled',?,?,?,?,?,0)";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", err);
            err = "db error";
            return false;
        }
        sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, list_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, list_rev);
        sqlite3_bind_int(st, 5, allow_leave ? 1 : 0);
        sqlite3_bind_text(st, 6, wave ? "wave" : "once", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 7, wave_size);
        sqlite3_bind_int(st, 8, wave_interval_s);
        sqlite3_bind_int(st, 9, fail_threshold_pct);
        sqlite3_bind_int(st, 10, total_waves);
        sqlite3_bind_int64(st, 11, start_ms);
        sqlite3_bind_int64(st, 12, deadline_ms);
        sqlite3_bind_int(st, 13, max_retries);
        sqlite3_bind_text(st, 14, actor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 15, now);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            exec("ROLLBACK", err);
            err = "db error";
            return false;
        }
    }

    for (int i = 0; i < n; ++i) {
        int w = i / wave_size + 1;
        const char* status = (w == 1) ? "scheduled" : "waiting";
        long long row_start = (w == 1) ? start_ms : 0;
        sqlite3_stmt* st = nullptr;
        const char* sql =
            "INSERT INTO distribution_tasks(batch_id,devid,kind,file_name,file_size,status,"
            " start_at_ms,deadline_at_ms,max_retries,actor,created_ms,payload_rev,wave)"
            " VALUES(?,?,'relay',?,0,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", err);
            err = "db error";
            return false;
        }
        sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, devids[i].c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, row_start);
        sqlite3_bind_int64(st, 6, deadline_ms);
        sqlite3_bind_int(st, 7, max_retries);
        sqlite3_bind_text(st, 8, actor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 9, now);
        sqlite3_bind_int64(st, 10, list_rev);
        sqlite3_bind_int(st, 11, w);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            exec("ROLLBACK", err);
            err = "db error";
            return false;
        }
    }
    std::string e;
    if (!exec("COMMIT", e)) { exec("ROLLBACK", e); err = "db error"; return false; }
    return true;
}

bool AdminDb::relayTaskActive(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT batch_id,name,list_rev,allow_leave,mode,wave_size,wave_interval_s,"
        " fail_threshold_pct,total_waves,current_wave,wave_done_ms,start_at_ms,deadline_at_ms,status"
        " FROM relay_sync_task WHERE status IN ('scheduled','running','paused') ORDER BY created_ms";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["batch"] = dbColText(st, 0);
        j["name"] = dbColText(st, 1);
        j["rev"] = (long long)sqlite3_column_int64(st, 2);
        j["allow_leave"] = sqlite3_column_int(st, 3) != 0;
        j["mode"] = dbColText(st, 4);
        j["wave_size"] = sqlite3_column_int(st, 5);
        j["wave_interval_s"] = sqlite3_column_int(st, 6);
        j["fail_threshold_pct"] = sqlite3_column_int(st, 7);
        j["total_waves"] = sqlite3_column_int(st, 8);
        j["current_wave"] = sqlite3_column_int(st, 9);
        j["wave_done_ms"] = (long long)sqlite3_column_int64(st, 10);
        j["start_at_ms"] = (long long)sqlite3_column_int64(st, 11);
        j["deadline_at_ms"] = (long long)sqlite3_column_int64(st, 12);
        j["status"] = dbColText(st, 13);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::relayTaskList(const std::string& status, const std::string& q, int page,
                            int page_size, const std::vector<std::string>& devids, bool scoped,
                            std::string& json_out, int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string where = " WHERE 1=1";
    std::vector<std::string> args;
    if (!status.empty()) {
        if (status == "active") where += " AND t.status IN ('scheduled','running')";
        else if (status == "done") where += " AND t.status='success'";
        else if (status == "failed_group") where += " AND t.status IN ('partial','failed')";
        else { where += " AND t.status=?"; args.push_back(status); }
    }
    if (!q.empty()) {
        where += " AND t.name LIKE ?";
        args.push_back("%" + q + "%");
    }
    if (scoped) {
        if (devids.empty()) {
            // 受限账号一台设备都没分配：不能退化成“不过滤”
            where += " AND 0";
        } else {
            where += " AND EXISTS(SELECT 1 FROM distribution_tasks i WHERE i.batch_id=t.batch_id"
                     " AND i.kind='relay' AND i.devid IN (";
            for (size_t i = 0; i < devids.size(); ++i) {
                if (i) where += ",";
                where += "?";
                args.push_back(devids[i]);
            }
            where += "))";
        }
    }
    if (page < 1) page = 1;
    if (page_size < 1 || page_size > 200) page_size = 20;

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, ("SELECT COUNT(*) FROM relay_sync_task t" + where).c_str(), -1, &st, nullptr) != SQLITE_OK)
        return false;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, (int)i + 1, args[i].c_str(), -1, SQLITE_TRANSIENT);
    total = 0;
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    std::string sql =
        "SELECT t.batch_id,t.name,t.list_rev,t.mode,t.total_waves,t.current_wave,t.status,"
        " t.start_at_ms,t.deadline_at_ms,t.actor,t.created_ms,t.finished_ms,t.allow_leave,"
        " (SELECT COUNT(*) FROM distribution_tasks i WHERE i.batch_id=t.batch_id AND i.kind='relay'),"
        " (SELECT COUNT(*) FROM distribution_tasks i WHERE i.batch_id=t.batch_id AND i.kind='relay' AND i.status='success'),"
        " (SELECT COUNT(*) FROM distribution_tasks i WHERE i.batch_id=t.batch_id AND i.kind='relay' AND i.status='failed'),"
        " (SELECT COUNT(*) FROM distribution_tasks i WHERE i.batch_id=t.batch_id AND i.kind='relay' AND i.status='timeout'),"
        " (SELECT COUNT(*) FROM distribution_tasks i WHERE i.batch_id=t.batch_id AND i.kind='relay' AND i.status='pending'),"
        " (SELECT COUNT(*) FROM distribution_tasks i WHERE i.batch_id=t.batch_id AND i.kind='relay' AND i.status IN ('scheduled','queued','running'))"
        " FROM relay_sync_task t" + where + " ORDER BY t.created_ms DESC LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, idx++, args[i].c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx++, page_size);
    sqlite3_bind_int(st, idx++, (page - 1) * page_size);

    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["batch"] = dbColText(st, 0);
        j["name"] = dbColText(st, 1);
        j["rev"] = (long long)sqlite3_column_int64(st, 2);
        j["mode"] = dbColText(st, 3);
        j["total_waves"] = sqlite3_column_int(st, 4);
        j["current_wave"] = sqlite3_column_int(st, 5);
        j["status"] = dbColText(st, 6);
        j["start_at_ms"] = (long long)sqlite3_column_int64(st, 7);
        j["deadline_at_ms"] = (long long)sqlite3_column_int64(st, 8);
        j["actor"] = dbColText(st, 9);
        j["created_ms"] = (long long)sqlite3_column_int64(st, 10);
        j["finished_ms"] = (long long)sqlite3_column_int64(st, 11);
        j["allow_leave"] = sqlite3_column_int(st, 12) != 0;
        j["total"] = sqlite3_column_int(st, 13);
        j["ok"] = sqlite3_column_int(st, 14);
        j["failed"] = sqlite3_column_int(st, 15);
        j["timeout"] = sqlite3_column_int(st, 16);
        j["pending"] = sqlite3_column_int(st, 17);
        j["active"] = sqlite3_column_int(st, 18);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    out["total"] = total;
    out["page"] = page;
    out["page_size"] = page_size;
    json_out = out.dump();
    return true;
}

bool AdminDb::relayTaskGet(const std::string& batch, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT batch_id,name,list_json,list_rev,allow_leave,mode,wave_size,wave_interval_s,"
        " fail_threshold_pct,total_waves,current_wave,wave_done_ms,status,start_at_ms,"
        " deadline_at_ms,max_retries,actor,created_ms,finished_ms"
        " FROM relay_sync_task WHERE batch_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "任务不存在";
        return false;
    }
    json j;
    j["batch"] = dbColText(st, 0);
    j["name"] = dbColText(st, 1);
    j["list_json"] = dbColText(st, 2);
    j["rev"] = (long long)sqlite3_column_int64(st, 3);
    j["allow_leave"] = sqlite3_column_int(st, 4) != 0;
    j["mode"] = dbColText(st, 5);
    j["wave_size"] = sqlite3_column_int(st, 6);
    j["wave_interval_s"] = sqlite3_column_int(st, 7);
    j["fail_threshold_pct"] = sqlite3_column_int(st, 8);
    j["total_waves"] = sqlite3_column_int(st, 9);
    j["current_wave"] = sqlite3_column_int(st, 10);
    j["wave_done_ms"] = (long long)sqlite3_column_int64(st, 11);
    j["status"] = dbColText(st, 12);
    j["start_at_ms"] = (long long)sqlite3_column_int64(st, 13);
    j["deadline_at_ms"] = (long long)sqlite3_column_int64(st, 14);
    j["max_retries"] = sqlite3_column_int(st, 15);
    j["actor"] = dbColText(st, 16);
    j["created_ms"] = (long long)sqlite3_column_int64(st, 17);
    j["finished_ms"] = (long long)sqlite3_column_int64(st, 18);
    sqlite3_finalize(st);
    // 任务级汇总（详情抽屉直接用，不必再拉一次列表）
    j["ok"] = countByStatus(db_, batch.c_str(), "'success'");
    j["failed"] = countByStatus(db_, batch.c_str(), "'failed'");
    j["timeout"] = countByStatus(db_, batch.c_str(), "'timeout'");
    j["pending"] = countByStatus(db_, batch.c_str(), "'pending'");
    j["active"] = countByStatus(db_, batch.c_str(), "'scheduled','queued','running'");
    j["total"] = itemCountLocked(db_, batch.c_str());
    json_out = j.dump();
    return true;
}

bool AdminDb::relayTaskItems(const std::string& batch, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,devid,wave,status,error,payload_rev,start_at_ms,started_ms,finished_ms,"
        " retry_count,max_retries FROM distribution_tasks"
        " WHERE batch_id=? AND kind='relay' ORDER BY wave,devid";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = (long long)sqlite3_column_int64(st, 0);
        j["devid"] = dbColText(st, 1);
        j["wave"] = sqlite3_column_int(st, 2);
        j["status"] = dbColText(st, 3);
        j["error"] = dbColText(st, 4);
        j["rev"] = (long long)sqlite3_column_int64(st, 5);
        j["start_at_ms"] = (long long)sqlite3_column_int64(st, 6);
        j["started_ms"] = (long long)sqlite3_column_int64(st, 7);
        j["finished_ms"] = (long long)sqlite3_column_int64(st, 8);
        j["retry_count"] = sqlite3_column_int(st, 9);
        j["max_retries"] = sqlite3_column_int(st, 10);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::relayTaskDue(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,batch_id,devid,status,start_at_ms,deadline_at_ms,retry_count,max_retries,"
        " next_retry_ms,started_ms,payload_rev,error,wave FROM distribution_tasks"
        " WHERE kind='relay' AND status IN ('scheduled','queued','running','pending')"
        " AND batch_id IN (SELECT batch_id FROM relay_sync_task WHERE status IN ('scheduled','running'))"
        " ORDER BY wave,id";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = (long long)sqlite3_column_int64(st, 0);
        j["batch"] = dbColText(st, 1);
        j["devid"] = dbColText(st, 2);
        j["status"] = dbColText(st, 3);
        j["start_at_ms"] = (long long)sqlite3_column_int64(st, 4);
        j["deadline_at_ms"] = (long long)sqlite3_column_int64(st, 5);
        j["retry_count"] = sqlite3_column_int(st, 6);
        j["max_retries"] = sqlite3_column_int(st, 7);
        j["next_retry_ms"] = (long long)sqlite3_column_int64(st, 8);
        j["started_ms"] = (long long)sqlite3_column_int64(st, 9);
        j["rev"] = (long long)sqlite3_column_int64(st, 10);
        j["error"] = dbColText(st, 11);
        j["wave"] = sqlite3_column_int(st, 12);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::relayTaskPayload(const std::string& batch, std::string& json_out, long long& rev) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT list_json,list_rev FROM relay_sync_task WHERE batch_id=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        json_out = dbColText(st, 0);
        rev = (long long)sqlite3_column_int64(st, 1);
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::relayTaskSetStatus(const std::string& batch, const std::string& status,
                                long long finished_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE relay_sync_task SET status=?, finished_ms=? WHERE batch_id=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, finished_ms);
    sqlite3_bind_text(st, 3, batch.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::relayTaskPauseStale(long long rev, int& n) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE relay_sync_task SET status='paused'"
                      " WHERE status IN ('scheduled','running') AND list_rev<>?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, rev);
    int rc = sqlite3_step(st);
    n = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::relayTaskSetWave(const std::string& batch, int wave, long long wave_done_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE relay_sync_task SET current_wave=?, wave_done_ms=? WHERE batch_id=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, wave);
    sqlite3_bind_int64(st, 2, wave_done_ms);
    sqlite3_bind_text(st, 3, batch.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::relayTaskReleaseWave(const std::string& batch, int wave, long long now_ms,
                                   std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status='scheduled', start_at_ms=?"
                      " WHERE batch_id=? AND kind='relay' AND wave=? AND status='waiting'";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, now_ms);
    sqlite3_bind_text(st, 2, batch.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, wave);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = "db error"; return false; }
    return true;
}

bool AdminDb::relayTaskWaveStats(const std::string& batch, int wave, int& success, int& failed,
                                 int& timeout, int& pending, int& active, int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    success = countByStatus(db_, batch.c_str(), "'success'", wave);
    failed = countByStatus(db_, batch.c_str(), "'failed'", wave);
    timeout = countByStatus(db_, batch.c_str(), "'timeout'", wave);
    pending = countByStatus(db_, batch.c_str(), "'pending'", wave);
    active = countByStatus(db_, batch.c_str(), "'scheduled','queued','running'", wave);
    total = countByStatus(db_, batch.c_str(),
                          "'success','failed','timeout','pending','cancelled','scheduled','queued','running'", wave);
    return true;
}

bool AdminDb::relayTaskOverall(const std::string& batch, int& success, int& failed, int& timeout,
                               int& pending, int& active, int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    success = countByStatus(db_, batch.c_str(), "'success'");
    failed = countByStatus(db_, batch.c_str(), "'failed'");
    timeout = countByStatus(db_, batch.c_str(), "'timeout'");
    pending = countByStatus(db_, batch.c_str(), "'pending'");
    active = countByStatus(db_, batch.c_str(), "'scheduled','waiting','queued','running'");
    total = itemCountLocked(db_, batch.c_str());
    return true;
}

bool AdminDb::relayTaskCancelPending(const std::string& batch, int& n) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status='cancelled', error='cancelled'"
                      " WHERE batch_id=? AND kind='relay'"
                      " AND status IN ('waiting','scheduled','queued','running','pending')";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    n = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::relayItemMarkRunning(long long id, long long started_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status='running', started_ms=?, error='' WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, started_ms);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::relayTaskRetryFailed(const std::string& batch, int& n, int& min_wave) {
    std::lock_guard<std::mutex> lk(mu_);
    min_wave = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "SELECT MIN(wave) FROM distribution_tasks WHERE batch_id=? AND kind='relay'"
                           " AND status IN ('failed','timeout')",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) min_wave = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    st = nullptr;
    // 手动重试给的是新一轮机会：重置重试计数，否则已用满的设备一超时就再次失败
    const char* sql = "UPDATE distribution_tasks SET status='queued', next_retry_ms=0, error='',"
                      " retry_count=0 WHERE batch_id=? AND kind='relay'"
                      " AND status IN ('failed','timeout')";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    n = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}
