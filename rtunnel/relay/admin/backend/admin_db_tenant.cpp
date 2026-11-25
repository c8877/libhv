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

}  // namespace

bool AdminDb::tenantList(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    const char* sql =
        "SELECT t.id,t.name,t.note,t.status,t.created_ms,"
        " (SELECT COUNT(*) FROM client_accounts a WHERE a.tenant_id=t.id),"
        " (SELECT COUNT(*) FROM res_devices d WHERE d.tenant_id=t.id),"
        " (SELECT COUNT(*) FROM res_devices d WHERE d.tenant_id=t.id AND d.online=1)"
        " FROM tenants t ORDER BY t.id";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
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
                           const std::string& status, std::string& err) {
    if (!validTenantName(name)) { err = "租户名需为 1-64 字节可见字符"; return false; }
    if (note.size() > 256) { err = "备注过长"; return false; }
    if (id == 1 && status == "disabled") { err = "默认租户不能停用"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE tenants SET name=?,note=?,status=?,updated_ms=? WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tenantStatusName(status).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, nowMs());
    sqlite3_bind_int64(st, 5, id);
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
