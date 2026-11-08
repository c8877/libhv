// 设备 TTY 配置
#include "admin_db.hpp"
#include "cpputil/json.hpp"

#include "sqlite3.h"

using nlohmann::json;

bool AdminDb::ttyList(const std::string& devid, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db_,
        "SELECT path,alias,enabled,source,baud,data_bits,parity,stop_bits,roles,updated_by,updated_ms"
        " FROM tty_config WHERE devid=? ORDER BY path", -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["path"] = (const char*)sqlite3_column_text(st, 0);
        j["alias"] = (const char*)sqlite3_column_text(st, 1);
        j["enabled"] = sqlite3_column_int(st, 2) != 0;
        j["source"] = (const char*)sqlite3_column_text(st, 3);
        j["baud"] = sqlite3_column_int(st, 4);
        j["data_bits"] = sqlite3_column_int(st, 5);
        j["parity"] = (const char*)sqlite3_column_text(st, 6);
        j["stop_bits"] = sqlite3_column_int(st, 7);
        j["roles"] = (const char*)sqlite3_column_text(st, 8);
        j["updated_by"] = (const char*)sqlite3_column_text(st, 9);
        j["updated_ms"] = sqlite3_column_int64(st, 10);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::ttyUpsert(const std::string& devid, const std::string& path, const std::string& alias,
                        int enabled, int baud, int data_bits, const std::string& parity, int stop_bits,
                        const std::string& roles, const std::string& actor, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (path.empty() || path[0] != '/') { err = "bad path"; return false; }
    if (!alias.empty()) {
        sqlite3_stmt* ck = NULL;
        sqlite3_prepare_v2(db_, "SELECT path FROM tty_config WHERE devid=? AND alias=? AND path<>?",
                           -1, &ck, NULL);
        sqlite3_bind_text(ck, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ck, 2, alias.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(ck, 3, path.c_str(), -1, SQLITE_TRANSIENT);
        bool dup = sqlite3_step(ck) == SQLITE_ROW;
        sqlite3_finalize(ck);
        if (dup) { err = "alias in use"; return false; }
    }
    const char* src = NULL;
    sqlite3_stmt* cur = NULL;
    sqlite3_prepare_v2(db_, "SELECT source FROM tty_config WHERE devid=? AND path=?", -1, &cur, NULL);
    sqlite3_bind_text(cur, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(cur, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    std::string source = "manual";
    if (sqlite3_step(cur) == SQLITE_ROW) {
        src = (const char*)sqlite3_column_text(cur, 0);
        if (src) source = src;
    }
    sqlite3_finalize(cur);

    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO tty_config(devid,path,alias,enabled,source,baud,data_bits,parity,stop_bits,roles,updated_by,updated_ms)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(devid,path) DO UPDATE SET alias=excluded.alias,enabled=excluded.enabled,"
        " baud=excluded.baud,data_bits=excluded.data_bits,parity=excluded.parity,"
        " stop_bits=excluded.stop_bits,roles=excluded.roles,updated_by=excluded.updated_by,"
        " updated_ms=excluded.updated_ms", -1, &st, NULL) != SQLITE_OK) { err = "db"; return false; }
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, alias.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, enabled);
    sqlite3_bind_text(st, 5, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, baud);
    sqlite3_bind_int(st, 7, data_bits);
    sqlite3_bind_text(st, 8, parity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 9, stop_bits);
    sqlite3_bind_text(st, 10, roles.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 11, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 12, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::ttyDelete(const std::string& devid, const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    sqlite3_prepare_v2(db_, "DELETE FROM tty_config WHERE devid=? AND path=?", -1, &st, NULL);
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) err = "db";
    return ok;
}

bool AdminDb::ttyResolve(const std::string& devid, const std::string& alias_or_path,
                         std::string& path, int& baud, int& data_bits,
                         std::string& parity, int& stop_bits, bool& enabled) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    sqlite3_prepare_v2(db_,
        "SELECT path,baud,data_bits,parity,stop_bits,enabled FROM tty_config"
        " WHERE devid=? AND (alias=? OR path=?) LIMIT 1", -1, &st, NULL);
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, alias_or_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, alias_or_path.c_str(), -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) {
        path = (const char*)sqlite3_column_text(st, 0);
        baud = sqlite3_column_int(st, 1);
        data_bits = sqlite3_column_int(st, 2);
        parity = (const char*)sqlite3_column_text(st, 3);
        stop_bits = sqlite3_column_int(st, 4);
        enabled = sqlite3_column_int(st, 5) != 0;
    }
    sqlite3_finalize(st);
    return found;
}
