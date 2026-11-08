// 设备采样配置
#include "admin_db.hpp"
#include "cpputil/json.hpp"

#include "sqlite3.h"

using nlohmann::json;

bool AdminDb::metricGet(const std::string& devid, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    bool found = false;
    json j;
    if (sqlite3_prepare_v2(db_,
        "SELECT enabled,interval_sec,points,metrics,interfaces,ttys,persist,updated_by,updated_ms"
        " FROM metric_config WHERE devid=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            found = true;
            j["enabled"] = sqlite3_column_int(st, 0) != 0;
            j["interval_sec"] = sqlite3_column_int(st, 1);
            j["points"] = sqlite3_column_int(st, 2);
            std::string m = (const char*)sqlite3_column_text(st, 3);
            json arr = json::array();
            size_t pos = 0;
            while (pos <= m.size()) {
                size_t c = m.find(',', pos);
                std::string one = m.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
                if (!one.empty()) arr.push_back(one);
                if (c == std::string::npos) break;
                pos = c + 1;
            }
            j["metrics"] = arr;
            auto split = [](const std::string& v) {
                json a = json::array();
                size_t pos = 0;
                while (pos <= v.size()) {
                    size_t c = v.find(',', pos);
                    std::string one = v.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
                    if (!one.empty()) a.push_back(one);
                    if (c == std::string::npos) break;
                    pos = c + 1;
                }
                return a;
            };
            j["interfaces"] = split((const char*)sqlite3_column_text(st, 4));
            j["ttys"] = split((const char*)sqlite3_column_text(st, 5));
            j["persist"] = sqlite3_column_int(st, 6) != 0;
            j["updated_by"] = (const char*)sqlite3_column_text(st, 7);
            j["updated_ms"] = sqlite3_column_int64(st, 8);
        }
        sqlite3_finalize(st);
    }
    if (!found) {
        j["enabled"] = true;
        j["interval_sec"] = 5;
        j["points"] = 720;
        j["metrics"] = json::array({"cpu", "mem", "load"});
        j["interfaces"] = json::array();
        j["ttys"] = json::array();
        j["persist"] = false;
        j["updated_by"] = "";
        j["updated_ms"] = 0;
        j["source"] = "default";
    } else {
        j["source"] = "admin";
    }
    json_out = j.dump();
    return true;
}

bool AdminDb::metricPut(const std::string& devid, int enabled, int interval_sec, int points,
                        const std::string& metrics, const std::string& ifaces, const std::string& ttys,
                        int persist, const std::string& actor, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db_,
        "INSERT INTO metric_config(devid,enabled,interval_sec,points,metrics,interfaces,ttys,persist,updated_by,updated_ms)"
        " VALUES(?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(devid) DO UPDATE SET enabled=excluded.enabled,interval_sec=excluded.interval_sec,"
        " points=excluded.points,metrics=excluded.metrics,interfaces=excluded.interfaces,"
        " ttys=excluded.ttys,persist=excluded.persist,updated_by=excluded.updated_by,updated_ms=excluded.updated_ms",
        -1, &st, NULL) != SQLITE_OK) { err = "db"; return false; }
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, enabled);
    sqlite3_bind_int(st, 3, interval_sec);
    sqlite3_bind_int(st, 4, points);
    sqlite3_bind_text(st, 5, metrics.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, ifaces.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, ttys.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 8, persist);
    sqlite3_bind_text(st, 9, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 10, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::metricDevids(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db_, "SELECT devid FROM metric_config", -1, &st, NULL) != SQLITE_OK) return false;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return true;
}
