// 部署目标主机与部署作业。
//
// 主机凭据不进库：密码/私钥写在 <admin.db 同目录>/deploy/<host_id>.secret（0600），
// 库里只记 auth 方式与 secret_set；接口永远不回显凭据。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <mutex>
#include <sqlite3.h>

using nlohmann::json;

namespace {
const char* kHostCols =
    "id,name,host,port,username,auth,sudo,note,secret_set,last_status,last_ms,created_ms,updated_ms";

bool hostRow(sqlite3_stmt* st, json& j) {
    j["id"] = sqlite3_column_int64(st, 0);
    j["name"] = dbColText(st, 1);
    j["host"] = dbColText(st, 2);
    j["port"] = sqlite3_column_int(st, 3);
    j["username"] = dbColText(st, 4);
    j["auth"] = dbColText(st, 5);
    j["sudo"] = sqlite3_column_int(st, 6) != 0;
    j["note"] = dbColText(st, 7);
    j["secret_set"] = sqlite3_column_int(st, 8) != 0;
    j["last_status"] = dbColText(st, 9);
    j["last_ms"] = (long long)sqlite3_column_int64(st, 10);
    j["created_ms"] = (long long)sqlite3_column_int64(st, 11);
    j["updated_ms"] = (long long)sqlite3_column_int64(st, 12);
    return true;
}
}  // namespace

bool AdminDb::deployHostList(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kHostCols + " FROM deploy_hosts ORDER BY updated_ms DESC, id DESC";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    json items = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        hostRow(st, j);
        items.push_back(j);
    }
    sqlite3_finalize(st);
    json out;
    out["items"] = items;
    json_out = out.dump();
    return true;
}

bool AdminDb::deployHostGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kHostCols + " FROM deploy_hosts WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        hostRow(st, j);
        json_out = j.dump();
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::deployHostSave(long long id, const std::string& name, const std::string& host,
                             int port, const std::string& user, const std::string& auth,
                             bool sudo, const std::string& note, long long& out_id,
                             std::string& err) {
    if (name.empty() || host.empty() || user.empty()) {
        err = "名称/地址/用户名不能为空";
        return false;
    }
    if (port <= 0 || port > 65535) {
        err = "端口不合法";
        return false;
    }
    if (auth != "password" && auth != "key") {
        err = "认证方式只能是 password 或 key";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (id > 0) {
        const char* sql =
            "UPDATE deploy_hosts SET name=?,host=?,port=?,username=?,auth=?,sudo=?,note=?,"
            "updated_ms=? WHERE id=?";
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
            err = "db error";
            return false;
        }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, host.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, port);
        sqlite3_bind_text(st, 4, user.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, auth.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 6, sudo ? 1 : 0);
        sqlite3_bind_text(st, 7, note.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 8, AdminDb::nowMs());
        sqlite3_bind_int64(st, 9, id);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) err = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
        out_id = id;
        return ok;
    }
    const char* sql =
        "INSERT INTO deploy_hosts(name,host,port,username,auth,sudo,note,created_ms,updated_ms)"
        " VALUES(?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, port);
    sqlite3_bind_text(st, 4, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, auth.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, sudo ? 1 : 0);
    sqlite3_bind_text(st, 7, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, AdminDb::nowMs());
    sqlite3_bind_int64(st, 9, AdminDb::nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    if (ok) out_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::deployHostSetSecret(long long id, bool set) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE deploy_hosts SET secret_set=?,updated_ms=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(st, 1, set ? 1 : 0);
    sqlite3_bind_int64(st, 2, AdminDb::nowMs());
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::deployHostDelete(long long id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM deploy_hosts WHERE id=?", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::deployHostMark(long long id, const std::string& status) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE deploy_hosts SET last_status=?,last_ms=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, AdminDb::nowMs());
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::deployJobCreate(long long host_id, const std::string& host, const std::string& user,
                              const std::string& component, const std::string& version,
                              const std::string& arch, const std::string& variant,
                              const std::string& pkg, const std::string& prefix,
                              const std::string& options, const std::string& actor,
                              long long& out_id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO deploy_jobs(host_id,host,username,component,version,arch,variant,pkg,prefix,"
        "options,status,step,log,error,actor,created_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    long long now = AdminDb::nowMs();
    sqlite3_bind_int64(st, 1, host_id);
    sqlite3_bind_text(st, 2, host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, component.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, arch.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, variant.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, pkg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, prefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, options.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, "queued", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 12, "排队", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 13, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 14, "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 15, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 16, now);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    if (ok) out_id = sqlite3_last_insert_rowid(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::deployJobUpdate(long long id, const std::string& status, const std::string& step,
                              const std::string& error, const std::string& log_add) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "UPDATE deploy_jobs SET status=?,step=?,error=?,"
        "log=substr(log || ?, -131072),"
        "started_ms=CASE WHEN started_ms=0 AND ?='running' THEN ? ELSE started_ms END,"
        "finished_ms=CASE WHEN ? IN ('success','failed','canceled') THEN ? ELSE finished_ms END"
        " WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    long long now = AdminDb::nowMs();
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, step.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, log_add.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, now);
    sqlite3_bind_text(st, 7, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 8, now);
    sqlite3_bind_int64(st, 9, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::deployJobRunningForHost(long long host_id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    bool running = false;
    if (sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM deploy_jobs WHERE host_id=? AND status IN ('queued','running')",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, host_id);
        if (sqlite3_step(st) == SQLITE_ROW) running = sqlite3_column_int64(st, 0) > 0;
    }
    sqlite3_finalize(st);
    return running;
}

bool AdminDb::deployJobGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,host_id,host,username,component,version,arch,variant,pkg,prefix,options,status,"
        "step,log,error,actor,created_ms,started_ms,finished_ms FROM deploy_jobs WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = sqlite3_column_int64(st, 0);
        j["host_id"] = sqlite3_column_int64(st, 1);
        j["host"] = dbColText(st, 2);
        j["username"] = dbColText(st, 3);
        j["component"] = dbColText(st, 4);
        j["version"] = dbColText(st, 5);
        j["arch"] = dbColText(st, 6);
        j["variant"] = dbColText(st, 7);
        j["pkg"] = dbColText(st, 8);
        j["prefix"] = dbColText(st, 9);
        j["options"] = dbColText(st, 10);
        j["status"] = dbColText(st, 11);
        j["step"] = dbColText(st, 12);
        j["log"] = dbColText(st, 13);
        j["error"] = dbColText(st, 14);
        j["actor"] = dbColText(st, 15);
        j["created_ms"] = (long long)sqlite3_column_int64(st, 16);
        j["started_ms"] = (long long)sqlite3_column_int64(st, 17);
        j["finished_ms"] = (long long)sqlite3_column_int64(st, 18);
        json_out = j.dump();
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::deployJobList(int limit, std::string& json_out) {
    if (limit <= 0) limit = 30;
    if (limit > 200) limit = 200;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT id,host_id,host,username,component,version,arch,variant,pkg,prefix,status,step,error,"
        "actor,created_ms,started_ms,finished_ms FROM deploy_jobs ORDER BY id DESC LIMIT ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, limit);
    json items = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = sqlite3_column_int64(st, 0);
        j["host_id"] = sqlite3_column_int64(st, 1);
        j["host"] = dbColText(st, 2);
        j["username"] = dbColText(st, 3);
        j["component"] = dbColText(st, 4);
        j["version"] = dbColText(st, 5);
        j["arch"] = dbColText(st, 6);
        j["variant"] = dbColText(st, 7);
        j["pkg"] = dbColText(st, 8);
        j["prefix"] = dbColText(st, 9);
        j["status"] = dbColText(st, 10);
        j["step"] = dbColText(st, 11);
        j["error"] = dbColText(st, 12);
        j["actor"] = dbColText(st, 13);
        j["created_ms"] = (long long)sqlite3_column_int64(st, 14);
        j["started_ms"] = (long long)sqlite3_column_int64(st, 15);
        j["finished_ms"] = (long long)sqlite3_column_int64(st, 16);
        items.push_back(j);
    }
    sqlite3_finalize(st);
    json out;
    out["items"] = items;
    json_out = out.dump();
    return true;
}
