// 告警事件：alertEnqueue 产生的每条告警在这里落一行，供「告警」页查看/确认。
//
// 只保留最近 2000 条：这是给人看与确认的队列，不是长期审计（长期记录在审计文件里）。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <mutex>
#include <sqlite3.h>

using nlohmann::json;

namespace {

const char* kCols = "id,at_ms,event,level,title,text,detail,acked_ms,acked_by";

json alertRow(sqlite3_stmt* st) {
    json j;
    j["id"] = (long long)sqlite3_column_int64(st, 0);
    j["at_ms"] = (long long)sqlite3_column_int64(st, 1);
    j["event"] = dbColText(st, 2);
    j["level"] = dbColText(st, 3);
    j["title"] = dbColText(st, 4);
    j["text"] = dbColText(st, 5);
    std::string d = dbColText(st, 6);
    if (!d.empty()) {
        json dj = json::parse(d, nullptr, false);
        if (!dj.is_discarded()) j["detail"] = dj;
    }
    j["acked_ms"] = (long long)sqlite3_column_int64(st, 7);
    j["acked_by"] = dbColText(st, 8);
    return j;
}

}  // namespace

bool AdminDb::alertInsert(long long at_ms, const std::string& event, const std::string& level,
                          const std::string& title, const std::string& text,
                          const std::string& detail) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO alert_events(at_ms,event,level,title,text,detail,acked_ms,acked_by)"
        " VALUES(?,?,?,?,?,?,0,'')";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, at_ms > 0 ? at_ms : nowMs());
    sqlite3_bind_text(st, 2, event.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, level.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, detail.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) return false;
    // 有界：只留最近 2000 条，避免长期运行后表无限增长
    sqlite3_exec(db_,
                 "DELETE FROM alert_events WHERE id <= "
                 "(SELECT MAX(id)-2000 FROM alert_events)",
                 nullptr, nullptr, nullptr);
    return true;
}

// state: open（未确认）/ acked（已确认）/ all
bool AdminDb::alertList(const std::string& state, int limit, std::string& json_out) {
    if (limit <= 0) limit = 100;
    if (limit > 500) limit = 500;
    std::lock_guard<std::mutex> lk(mu_);

    json counts;
    {
        sqlite3_stmt* c = nullptr;
        const char* csql =
            "SELECT COUNT(*),"
            " SUM(CASE WHEN acked_ms=0 THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN acked_ms=0 AND level='critical' THEN 1 ELSE 0 END),"
            " SUM(CASE WHEN acked_ms=0 AND level='warning' THEN 1 ELSE 0 END)"
            " FROM alert_events";
        if (sqlite3_prepare_v2(db_, csql, -1, &c, nullptr) == SQLITE_OK) {
            if (sqlite3_step(c) == SQLITE_ROW) {
                counts["total"] = (long long)sqlite3_column_int64(c, 0);
                counts["open"] = (long long)sqlite3_column_int64(c, 1);
                counts["critical"] = (long long)sqlite3_column_int64(c, 2);
                counts["warning"] = (long long)sqlite3_column_int64(c, 3);
            }
            sqlite3_finalize(c);
        }
    }

    std::string where;
    if (state == "open") where = " WHERE acked_ms=0";
    else if (state == "acked") where = " WHERE acked_ms>0";
    std::string sql = std::string("SELECT ") + kCols + " FROM alert_events" + where +
                      " ORDER BY id DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int(st, 1, limit);
    json items = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) items.push_back(alertRow(st));
    sqlite3_finalize(st);

    json out;
    out["items"] = items;
    out["counts"] = counts;
    json_out = out.dump();
    return true;
}

bool AdminDb::alertAck(long long id, const std::string& actor, bool& found) {
    found = false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE alert_events SET acked_ms=?, acked_by=? WHERE id=? AND acked_ms=0";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, nowMs());
    sqlite3_bind_text(st, 2, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, id);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    found = ok && sqlite3_changes(db_) > 0;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::alertAckAll(const std::string& actor, int& acked) {
    acked = 0;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE alert_events SET acked_ms=?, acked_by=? WHERE acked_ms=0";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, nowMs());
    sqlite3_bind_text(st, 2, actor.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (ok) acked = sqlite3_changes(db_);
    sqlite3_finalize(st);
    return ok;
}
