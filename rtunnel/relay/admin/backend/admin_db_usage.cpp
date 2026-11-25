// 用量流水（append-only）与日聚合：按量计费的数据底座。
// relay 只负责在内存里计数，由管理面 worker 周期性取走并写入这里。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <ctime>
#include <sqlite3.h>

using nlohmann::json;

namespace {

// 事件归属日按 relay 所在时区算，报表和"日"的直觉一致
std::string localDay(long long ts_ms) {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return std::string(buf);
}

bool isDay(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

void bindText(sqlite3_stmt* st, int idx, const std::string& v) {
    sqlite3_bind_text(st, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}

}  // namespace

bool AdminDb::usageAppend(const std::string& events_json, std::string& err) {
    json arr = json::parse(events_json, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) { err = "bad events"; return false; }
    if (arr.empty()) return true;

    std::lock_guard<std::mutex> lk(mu_);
    if (!exec("BEGIN IMMEDIATE", err)) return false;

    sqlite3_stmt* ev = nullptr;
    sqlite3_stmt* day = nullptr;
    auto fail = [&](const char* why) {
        if (ev) sqlite3_finalize(ev);
        if (day) sqlite3_finalize(day);
        exec("ROLLBACK", err);
        err = why;
        return false;
    };
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO usage_events(ts_ms,day,subject_type,subject_id,metric,qty,meta,source)"
            " VALUES(?,?,?,?,?,?,?,?)", -1, &ev, nullptr) != SQLITE_OK) {
        return fail("db error");
    }
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO usage_daily(day,subject_type,subject_id,metric,qty,events) VALUES(?,?,?,?,?,1)"
            " ON CONFLICT(day,subject_type,subject_id,metric)"
            " DO UPDATE SET qty=qty+excluded.qty, events=events+1", -1, &day, nullptr) != SQLITE_OK) {
        return fail("db error");
    }

    for (auto& e : arr) {
        if (!e.is_object()) continue;
        long long ts = e.value("ts_ms", (long long)0);
        std::string d = e.value("day", std::string());
        std::string subject_type = e.value("subject_type", std::string());
        std::string subject_id = e.value("subject_id", std::string());
        std::string metric = e.value("metric", std::string());
        long long qty = e.value("qty", (long long)0);
        std::string source = e.value("source", std::string("relay"));
        std::string meta;
        if (e.contains("meta")) {
            if (e["meta"].is_string()) meta = e["meta"].get<std::string>();
            else meta = e["meta"].dump();
        }
        // 主体/指标缺失或数值为零的事件没有意义（0 秒会话、0 字节 flush）
        if (subject_type.empty() || subject_id.empty() || metric.empty() || qty == 0) continue;
        if (d.empty()) d = localDay(ts);
        if (!isDay(d)) continue;
        if (source.empty()) source = "relay";

        sqlite3_reset(ev);
        sqlite3_clear_bindings(ev);
        sqlite3_bind_int64(ev, 1, ts);
        bindText(ev, 2, d);
        bindText(ev, 3, subject_type);
        bindText(ev, 4, subject_id);
        bindText(ev, 5, metric);
        sqlite3_bind_int64(ev, 6, qty);
        bindText(ev, 7, meta);
        bindText(ev, 8, source);
        if (sqlite3_step(ev) != SQLITE_DONE) return fail("insert event failed");

        sqlite3_reset(day);
        sqlite3_clear_bindings(day);
        bindText(day, 1, d);
        bindText(day, 2, subject_type);
        bindText(day, 3, subject_id);
        bindText(day, 4, metric);
        sqlite3_bind_int64(day, 5, qty);
        if (sqlite3_step(day) != SQLITE_DONE) return fail("rollup failed");
    }

    sqlite3_finalize(ev);
    sqlite3_finalize(day);
    if (!exec("COMMIT", err)) return false;
    return true;
}

bool AdminDb::usageQuery(const std::string& from, const std::string& to,
                         const std::string& subject_type, const std::string& subject_id,
                         const std::string& metric, bool total, std::string& json_out,
                         std::string& err) {
    if ((!from.empty() && !isDay(from)) || (!to.empty() && !isDay(to))) {
        err = "bad date";
        return false;
    }
    std::string where = " WHERE 1=1";
    if (!from.empty()) where += " AND day>=?";
    if (!to.empty()) where += " AND day<=?";
    if (!subject_type.empty()) where += " AND subject_type=?";
    if (!subject_id.empty()) where += " AND subject_id=?";
    if (!metric.empty()) where += " AND metric=?";

    std::string sql;
    if (total) {
        sql = "SELECT subject_type,subject_id,metric,SUM(qty),SUM(events) FROM usage_daily" + where +
              " GROUP BY subject_type,subject_id,metric ORDER BY SUM(qty) DESC LIMIT 1000";
    } else {
        sql = "SELECT day,subject_type,subject_id,metric,qty,events FROM usage_daily" + where +
              " ORDER BY day, subject_type, subject_id, metric LIMIT 20000";
    }

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    int idx = 1;
    if (!from.empty()) bindText(st, idx++, from);
    if (!to.empty()) bindText(st, idx++, to);
    if (!subject_type.empty()) bindText(st, idx++, subject_type);
    if (!subject_id.empty()) bindText(st, idx++, subject_id);
    if (!metric.empty()) bindText(st, idx++, metric);

    json rows = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json r;
        if (total) {
            r["subject_type"] = (const char*)sqlite3_column_text(st, 0);
            r["subject_id"] = (const char*)sqlite3_column_text(st, 1);
            r["metric"] = (const char*)sqlite3_column_text(st, 2);
            r["qty"] = sqlite3_column_int64(st, 3);
            r["events"] = sqlite3_column_int64(st, 4);
        } else {
            r["day"] = (const char*)sqlite3_column_text(st, 0);
            r["subject_type"] = (const char*)sqlite3_column_text(st, 1);
            r["subject_id"] = (const char*)sqlite3_column_text(st, 2);
            r["metric"] = (const char*)sqlite3_column_text(st, 3);
            r["qty"] = sqlite3_column_int64(st, 4);
            r["events"] = sqlite3_column_int64(st, 5);
        }
        rows.push_back(r);
    }
    sqlite3_finalize(st);

    json out;
    out["rows"] = rows;
    if (!from.empty()) out["from"] = from;
    if (!to.empty()) out["to"] = to;
    out["granularity"] = total ? "total" : "day";
    json_out = out.dump();
    return true;
}

bool AdminDb::usagePrune(long long before_ms, int& n) {
    n = 0;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM usage_events WHERE ts_ms<?", -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, before_ms);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (ok) n = sqlite3_changes(db_);
    return ok;
}
