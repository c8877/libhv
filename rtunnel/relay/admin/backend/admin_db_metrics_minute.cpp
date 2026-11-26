// 分钟级设备指标：relay 每分钟采一条 cpu/内存/负载/网卡字节，供趋势与异常检测使用。
// 只保留聚合后的分钟点（7 天默认），原始采样不落库。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <sqlite3.h>

#include <string>

using nlohmann::json;

bool AdminDb::metricsAppend(const std::string& json_in, int& rows, std::string& err) {
    rows = 0;
    json arr = json::parse(json_in, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) {
        err = "bad json";
        return false;
    }
    if (arr.empty()) return true;

    std::lock_guard<std::mutex> lk(mu_);
    if (!exec("BEGIN IMMEDIATE", err)) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO metrics_minute(devid,ts_min,cpu,mem,mem_total,load1,rx,tx)"
            " VALUES(?,?,?,?,?,?,?,?)"
            " ON CONFLICT(devid,ts_min) DO UPDATE SET"
            " cpu=excluded.cpu,mem=excluded.mem,mem_total=excluded.mem_total,"
            " load1=excluded.load1,rx=excluded.rx,tx=excluded.tx",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        exec("ROLLBACK", err);
        return false;
    }
    for (auto& r : arr) {
        if (!r.is_object()) continue;
        std::string devid = r.value("devid", std::string());
        long long ts = r.value("ts", (long long)0);
        if (devid.empty() || ts <= 0) continue;
        long long ts_min = ts / 1000;   // 秒；同一分钟内重复采样取最后一次
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, ts_min);
        sqlite3_bind_double(st, 3, r.value("cpu", 0.0));
        sqlite3_bind_int64(st, 4, r.value("mem", (long long)0));
        sqlite3_bind_int64(st, 5, r.value("mem_total", (long long)0));
        sqlite3_bind_double(st, 6, r.value("load1", 0.0));
        sqlite3_bind_int64(st, 7, r.value("rx", (long long)0));
        sqlite3_bind_int64(st, 8, r.value("tx", (long long)0));
        if (sqlite3_step(st) == SQLITE_DONE) ++rows;
    }
    sqlite3_finalize(st);
    if (!exec("COMMIT", err)) {
        exec("ROLLBACK", err);
        return false;
    }
    return true;
}

// step_sec 必须是 60 的整数倍：按 (ts_min / step) 分桶后取平均，
// 顺带把网卡计数取桶内最大值（计数器是单调的，平均值没有意义）。
bool AdminDb::metricsQuery(const std::string& devid, long long from_ms, long long to_ms,
                           int step_sec, std::string& json_out, std::string& err) {
    if (devid.empty()) {
        err = "devid required";
        return false;
    }
    if (step_sec < 60) step_sec = 60;
    step_sec -= step_sec % 60;
    long long from_s = from_ms / 1000;
    long long to_s = to_ms / 1000;
    if (from_s <= 0 || to_s <= 0 || to_s < from_s) {
        err = "bad range";
        return false;
    }
    if (to_s - from_s > 31LL * 86400) {
        err = "range too large (max 31 days)";
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT (ts_min/?)*? AS bucket, AVG(cpu), AVG(mem), MAX(mem_total), AVG(load1),"
        " MAX(rx), MAX(tx), COUNT(*)"
        " FROM metrics_minute WHERE devid=? AND ts_min>=? AND ts_min<=?"
        " GROUP BY bucket ORDER BY bucket";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int(st, 1, step_sec);
    sqlite3_bind_int(st, 2, step_sec);
    sqlite3_bind_text(st, 3, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, from_s);
    sqlite3_bind_int64(st, 5, to_s);

    json points = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json p;
        p["ts"] = (long long)sqlite3_column_int64(st, 0) * 1000;
        p["cpu"] = sqlite3_column_double(st, 1);
        p["mem"] = (long long)sqlite3_column_int64(st, 2);
        p["mem_total"] = (long long)sqlite3_column_int64(st, 3);
        p["load1"] = sqlite3_column_double(st, 4);
        p["rx"] = (long long)sqlite3_column_int64(st, 5);
        p["tx"] = (long long)sqlite3_column_int64(st, 6);
        p["samples"] = (long long)sqlite3_column_int64(st, 7);
        points.push_back(p);
    }
    sqlite3_finalize(st);

    json out;
    out["devid"] = devid;
    out["from"] = from_ms;
    out["to"] = to_ms;
    out["step"] = step_sec;
    out["points"] = std::move(points);
    json_out = out.dump();
    return true;
}

bool AdminDb::metricsPrune(long long before_ms, int& n) {
    n = 0;
    long long before_s = before_ms / 1000;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM metrics_minute WHERE ts_min < ?", -1, &st, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, before_s);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (ok) n = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return ok;
}
