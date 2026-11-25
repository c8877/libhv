// 租户：计费与设备归属边界。默认租户 id=1，升级前的数据都在它下面。
// 隔离点：接入账号带 tenant_id；设备带 tenant_id；ACL 规则带租户，relay 在 OPEN 时比对。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <sqlite3.h>

using nlohmann::json;

namespace {

bool validTenantName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

std::string tenantStatusName(const std::string& s) {
    return s == "disabled" ? "disabled" : "active";
}

// 当月首日（本地时区），与用量报表的 day 口径一致
std::string monthStartDay() {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    tmv.tm_mday = 1;
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    mktime(&tmv);
    char buf[16] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return std::string(buf);
}

}  // namespace

bool AdminDb::tenantList(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    const char* sql =
        "SELECT t.id,t.name,t.note,t.status,t.created_ms,"
        " (SELECT COUNT(*) FROM client_accounts a WHERE a.tenant_id=t.id),"
        " (SELECT COUNT(*) FROM res_devices d WHERE d.tenant_id=t.id),"
        " (SELECT COUNT(*) FROM res_devices d WHERE d.tenant_id=t.id AND d.online=1),"
        " t.max_devices,t.max_sessions,t.quota_bytes,"
        " (SELECT COALESCE(SUM(u.qty),0) FROM usage_daily u"
        "   WHERE u.tenant_id=t.id AND u.metric='bytes' AND u.day>=?)"
        " FROM tenants t ORDER BY t.id";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    std::string mstart = monthStartDay();
    sqlite3_bind_text(st, 1, mstart.c_str(), -1, SQLITE_TRANSIENT);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json t;
        t["id"] = (long long)sqlite3_column_int64(st, 0);
        t["name"] = (const char*)sqlite3_column_text(st, 1);
        t["note"] = (const char*)sqlite3_column_text(st, 2);
        t["status"] = (const char*)sqlite3_column_text(st, 3);
        t["created_ms"] = (long long)sqlite3_column_int64(st, 4);
        t["accounts"] = (int)sqlite3_column_int(st, 5);
        t["devices"] = (int)sqlite3_column_int(st, 6);
        t["online"] = (int)sqlite3_column_int(st, 7);
        t["max_devices"] = (int)sqlite3_column_int(st, 8);
        t["max_sessions"] = (int)sqlite3_column_int(st, 9);
        t["quota_bytes"] = (long long)sqlite3_column_int64(st, 10);
        t["used_bytes_month"] = (long long)sqlite3_column_int64(st, 11);
        arr.push_back(t);
    }
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    json_out = out.dump();
    return true;
}

bool AdminDb::tenantCreate(const std::string& name, const std::string& note, const std::string& by,
                           long long& id, std::string& err) {
    if (!validTenantName(name)) { err = "租户名需为 1-64 字节可见字符"; return false; }
    if (note.size() > 256) { err = "备注过长"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO tenants(name,note,status,created_ms,updated_ms) VALUES(?,?,'active',?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = "db error"; return false; }
    long long now = nowMs();
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) {
        std::string e = sqlite3_errmsg(db_);
        err = e.find("UNIQUE") != std::string::npos ? "租户名已存在" : e;
    }
    sqlite3_finalize(st);
    if (!ok) return false;
    id = sqlite3_last_insert_rowid(db_);
    (void)by;
    return true;
}

bool AdminDb::tenantUpdate(long long id, const std::string& name, const std::string& note,
                           const std::string& status, int max_devices, int max_sessions,
                           long long quota_bytes, std::string& err) {
    if (!validTenantName(name)) { err = "租户名需为 1-64 字节可见字符"; return false; }
    if (note.size() > 256) { err = "备注过长"; return false; }
    if (id == 1 && status == "disabled") { err = "默认租户不能停用"; return false; }
    // -1 表示“保持原值”，便于只改名称/状态的调用方
    if (max_devices < -1 || max_sessions < -1 || quota_bytes < -1) { err = "配额不能为负"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE tenants SET name=?,note=?,status=?,"
            "max_devices=CASE WHEN ?<0 THEN max_devices ELSE ? END,"
            "max_sessions=CASE WHEN ?<0 THEN max_sessions ELSE ? END,"
            "quota_bytes=CASE WHEN ?<0 THEN quota_bytes ELSE ? END,"
            "updated_ms=? WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tenantStatusName(status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, max_devices);
    sqlite3_bind_int(st, 5, max_devices < 0 ? 0 : max_devices);
    sqlite3_bind_int(st, 6, max_sessions);
    sqlite3_bind_int(st, 7, max_sessions < 0 ? 0 : max_sessions);
    sqlite3_bind_int64(st, 8, quota_bytes);
    sqlite3_bind_int64(st, 9, quota_bytes < 0 ? 0 : quota_bytes);
    sqlite3_bind_int64(st, 10, nowMs());
    sqlite3_bind_int64(st, 11, id);
    bool done = sqlite3_step(st) == SQLITE_DONE;
    int changed = done ? sqlite3_changes(db_) : 0;
    std::string e = done ? std::string() : sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (!done) {
        err = e.find("UNIQUE") != std::string::npos ? "租户名已存在" : e;
        return false;
    }
    if (changed == 0) { err = "租户不存在"; return false; }
    return true;
}

bool AdminDb::tenantExists(long long id) {
    if (id <= 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM tenants WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        ok = sqlite3_step(st) == SQLITE_ROW;
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::tenantOfClient(long long account_id, long long& out) {
    out = 0;
    if (account_id <= 0) return false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, "SELECT tenant_id FROM client_accounts WHERE id=?", -1, &st, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int64(st, 1, account_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out = (long long)sqlite3_column_int64(st, 0);
            ok = true;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::tenantDevices(long long tenant_id, std::vector<std::string>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid FROM res_devices WHERE tenant_id=? ORDER BY devid", -1, &st,
                           nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, tenant_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* s = (const char*)sqlite3_column_text(st, 0);
        if (s) out.emplace_back(s);
    }
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::deviceTenants(std::map<std::string, long long>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid,tenant_id FROM res_devices", -1, &st, nullptr) != SQLITE_OK)
        return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char* s = (const char*)sqlite3_column_text(st, 0);
        if (s) out[s] = (long long)sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::deviceTenant(const std::string& devid, long long& out) {
    out = 0;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db_, "SELECT tenant_id FROM res_devices WHERE devid=?", -1, &st, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            out = (long long)sqlite3_column_int64(st, 0);
            ok = true;
        }
    }
    sqlite3_finalize(st);
    return ok;
}

// 设备数上限：按已登记设备计；默认租户是运营方自有设备，不受限
static bool deviceCapLocked(sqlite3* db, const std::string& devid, long long tenant_id,
                            std::string& err) {
    if (tenant_id == 1) return true;
    sqlite3_stmt* q = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT t.max_devices,"
            " (SELECT COUNT(*) FROM res_devices d WHERE d.tenant_id=t.id AND d.devid<>?),"
            " (SELECT tenant_id FROM res_devices WHERE devid=?)"
            " FROM tenants t WHERE t.id=?",
            -1, &q, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(q, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(q, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(q, 3, tenant_id);
    bool ok = true;
    if (sqlite3_step(q) == SQLITE_ROW) {
        int max_dev = (int)sqlite3_column_int(q, 0);
        int used = (int)sqlite3_column_int(q, 1);
        long long cur = (long long)sqlite3_column_int64(q, 2);
        if (max_dev > 0 && cur != tenant_id && used >= max_dev) {
            err = "该租户设备数已达上限";
            ok = false;
        }
    }
    sqlite3_finalize(q);
    return ok;
}

bool AdminDb::tenantDeviceFit(const std::string& devid, long long tenant_id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    return deviceCapLocked(db_, devid, tenant_id, err);
}

bool AdminDb::setDeviceTenant(const std::string& devid, long long tenant_id, std::string& err) {
    if (devid.empty()) { err = "bad devid"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1 FROM tenants WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, tenant_id);
    bool exists = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    if (!exists) { err = "租户不存在"; return false; }
    if (!deviceCapLocked(db_, devid, tenant_id, err)) return false;

    st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE res_devices SET tenant_id=?,updated_ms=? WHERE devid=?", -1, &st,
                           nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, tenant_id);
    sqlite3_bind_int64(st, 2, nowMs());
    sqlite3_bind_text(st, 3, devid.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    sqlite3_finalize(st);
    if (!ok) err = "设备不存在";
    return ok;
}
