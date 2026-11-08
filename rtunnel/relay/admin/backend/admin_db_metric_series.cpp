// 分指标采样配置
#include "admin_db.hpp"
#include "cpputil/json.hpp"

#include "sqlite3.h"

using nlohmann::json;

bool AdminDb::metricSeriesList(const std::string& devid, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    json arr = json::array();
    if (sqlite3_prepare_v2(db_,
        "SELECT series,enabled,interval_sec,points,revision,sync_state,updated_by,updated_ms,synced_ms"
        " FROM metric_series_config WHERE devid=? ORDER BY series", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            json j;
            j["series"] = (const char*)sqlite3_column_text(st, 0);
            j["enabled"] = sqlite3_column_int(st, 1) != 0;
            j["interval_sec"] = sqlite3_column_int(st, 2);
            j["points"] = sqlite3_column_int(st, 3);
            j["revision"] = sqlite3_column_int64(st, 4);
            j["synced"] = sqlite3_column_int(st, 5) != 0;
            j["updated_by"] = (const char*)sqlite3_column_text(st, 6);
            j["updated_ms"] = sqlite3_column_int64(st, 7);
            j["synced_ms"] = sqlite3_column_int64(st, 8);
            arr.push_back(j);
        }
        sqlite3_finalize(st);
    }
    if (arr.empty()) {
        for (const char* name : {"cpu", "mem", "load"}) {
            json j;
            j["series"] = name;
            j["enabled"] = true;
            j["interval_sec"] = 5;
            j["points"] = 720;
            j["revision"] = 0;
            j["synced"] = false;
            j["updated_by"] = "";
            j["updated_ms"] = 0;
            j["synced_ms"] = 0;
            j["source"] = "default";
            arr.push_back(j);
        }
    }
    json_out = arr.dump();
    return true;
}

bool AdminDb::metricSeriesPut(const std::string& devid, const std::string& rows_json,
                              const std::string& actor, long long& revision, std::string& err) {
    auto rows = json::parse(rows_json, nullptr, false);
    if (rows.is_discarded() || !rows.is_array() || rows.empty()) { err = "bad config"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* mx = NULL;
    revision = 1;
    if (sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(revision),0)+1 FROM metric_series_config WHERE devid=?",
                           -1, &mx, NULL) == SQLITE_OK) {
        sqlite3_bind_text(mx, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(mx) == SQLITE_ROW) revision = sqlite3_column_int64(mx, 0);
        sqlite3_finalize(mx);
    }
    exec("BEGIN", err);
    sqlite3_stmt* del = NULL;
    sqlite3_prepare_v2(db_, "DELETE FROM metric_series_config WHERE devid=?", -1, &del, NULL);
    sqlite3_bind_text(del, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del);
    sqlite3_finalize(del);
    for (auto& r : rows) {
        if (!r.is_object()) continue;
        std::string series = r.value("series", "");
        if (series.empty()) continue;
        int enabled = r.value("enabled", true) ? 1 : 0;
        int interval = r.value("interval_sec", 5);
        int points = r.value("points", 720);
        if (interval < 1 || interval > 300 || points < 60 || points > 86400) continue;
        sqlite3_stmt* st = NULL;
        if (sqlite3_prepare_v2(db_,
            "INSERT INTO metric_series_config(devid,series,enabled,interval_sec,points,revision,sync_state,updated_by,updated_ms,synced_ms)"
            " VALUES(?,?,?,?,?,?,0,?,?,0)", -1, &st, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, series.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, enabled);
        sqlite3_bind_int(st, 4, interval);
        sqlite3_bind_int(st, 5, points);
        sqlite3_bind_int64(st, 6, revision);
        sqlite3_bind_text(st, 7, actor.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 8, nowMs());
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    exec("COMMIT", err);
    return true;
}

bool AdminDb::metricSeriesDevids(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT devid FROM metric_series_config", -1, &st, NULL) != SQLITE_OK) return false;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::metricSeriesAck(const std::string& devid, long long revision) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db_,
        "UPDATE metric_series_config SET sync_state=1,synced_ms=? WHERE devid=? AND revision=?",
        -1, &st, NULL) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, nowMs());
    sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, revision);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
