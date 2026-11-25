// 价目与费用试算：用量（usage_daily）× 单价。
// 只做“本月大概多少钱”的核算，不产生发票；金额单位是 1e-6 元，避免浮点。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <sqlite3.h>

#include <map>

using nlohmann::json;

namespace {

bool isDay(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

// 只允许已计量的四个指标，避免价目表里出现永远不会被引用的键
bool knownMetric(const std::string& m) {
    return m == "bytes" || m == "sessions" || m == "session_seconds" || m == "online_seconds";
}

// qty * price / unit，整数运算避免溢出：先取整商，再算余数部分
long long amountMicro(long long qty, long long price_micro, long long unit_size) {
    if (qty <= 0 || price_micro <= 0 || unit_size <= 0) return 0;
    long long q = qty / unit_size;
    long long r = qty % unit_size;
    return q * price_micro + r * price_micro / unit_size;
}

}  // namespace

bool AdminDb::billingPrices(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT metric,unit_size,price_micro,currency,updated_ms FROM price_rules"
            " ORDER BY metric", -1, &st, nullptr) != SQLITE_OK)
        return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["metric"] = dbColText(st, 0);
        j["unit_size"] = dbColInt(st, 1);
        j["price_micro"] = dbColInt(st, 2);
        j["currency"] = dbColText(st, 3);
        j["updated_ms"] = dbColInt(st, 4);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    json_out = out.dump();
    return true;
}

bool AdminDb::billingPriceSet(const std::string& metric, long long unit_size, long long price_micro,
                              const std::string& currency, std::string& err) {
    if (!knownMetric(metric)) { err = "未知指标"; return false; }
    if (unit_size <= 0) { err = "计价单位必须大于 0"; return false; }
    if (price_micro < 0) { err = "单价不能为负"; return false; }
    if (currency.size() > 8) { err = "币种过长"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO price_rules(metric,unit_size,price_micro,currency,updated_ms)"
            " VALUES(?,?,?,?,?) ON CONFLICT(metric) DO UPDATE SET"
            " unit_size=excluded.unit_size,price_micro=excluded.price_micro,"
            " currency=excluded.currency,updated_ms=excluded.updated_ms",
            -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, metric.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, unit_size);
    sqlite3_bind_int64(st, 3, price_micro);
    sqlite3_bind_text(st, 4, currency.empty() ? "CNY" : currency.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::billingSummary(const std::string& from, const std::string& to, long long tenant_id,
                             std::string& json_out, std::string& err) {
    if ((!from.empty() && !isDay(from)) || (!to.empty() && !isDay(to))) {
        err = "bad date";
        return false;
    }

    struct Price {
        long long unit_size = 1;
        long long price_micro = 0;
    };
    std::map<std::string, Price> prices;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT metric,unit_size,price_micro FROM price_rules", -1, &st,
                               nullptr) != SQLITE_OK) {
            err = "db error";
            return false;
        }
        while (sqlite3_step(st) == SQLITE_ROW) {
            Price p;
            p.unit_size = dbColInt(st, 1);
            p.price_micro = dbColInt(st, 2);
            if (p.unit_size <= 0) p.unit_size = 1;
            prices[dbColText(st, 0)] = p;
        }
        sqlite3_finalize(st);
    }

    std::string where = " WHERE 1=1";
    if (!from.empty()) where += " AND u.day>=?";
    if (!to.empty()) where += " AND u.day<=?";
    if (tenant_id > 0) where += " AND u.tenant_id=?";
    std::string sql =
        "SELECT u.tenant_id,COALESCE(t.name,'未归属'),u.metric,SUM(u.qty)"
        " FROM usage_daily u LEFT JOIN tenants t ON t.id=u.tenant_id" + where +
        " GROUP BY u.tenant_id,u.metric ORDER BY u.tenant_id,u.metric";

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    int idx = 1;
    if (!from.empty()) sqlite3_bind_text(st, idx++, from.c_str(), -1, SQLITE_TRANSIENT);
    if (!to.empty()) sqlite3_bind_text(st, idx++, to.c_str(), -1, SQLITE_TRANSIENT);
    if (tenant_id > 0) sqlite3_bind_int64(st, idx++, tenant_id);

    // 按租户聚合，租户内按指标
    std::map<long long, json> by_tenant;
    std::map<long long, std::string> names;
    std::map<long long, long long> totals;
    while (sqlite3_step(st) == SQLITE_ROW) {
        long long tid = sqlite3_column_int64(st, 0);
        std::string name = (const char*)sqlite3_column_text(st, 1);
        std::string metric = (const char*)sqlite3_column_text(st, 2);
        long long qty = sqlite3_column_int64(st, 3);
        Price p = prices.count(metric) ? prices[metric] : Price{};
        long long amount = amountMicro(qty, p.price_micro, p.unit_size);
        json item;
        item["metric"] = metric;
        item["qty"] = qty;
        item["unit_size"] = p.unit_size;
        item["price_micro"] = p.price_micro;
        item["amount_micro"] = amount;
        if (!by_tenant.count(tid)) by_tenant[tid] = json::array();
        by_tenant[tid].push_back(item);
        names[tid] = name;
        totals[tid] += amount;
    }
    sqlite3_finalize(st);

    json tenants = json::array();
    long long grand = 0;
    std::string currency = "CNY";
    for (auto& kv : by_tenant) {
        json t;
        t["tenant_id"] = kv.first;
        t["tenant_name"] = names[kv.first];
        t["items"] = kv.second;
        t["total_micro"] = totals[kv.first];
        tenants.push_back(t);
        grand += totals[kv.first];
    }
    json out;
    if (!from.empty()) out["from"] = from;
    if (!to.empty()) out["to"] = to;
    out["currency"] = currency;
    out["tenants"] = tenants;
    out["total_micro"] = grand;
    json_out = out.dump();
    return true;
}

// ---- 结算单 ----

namespace {

bool isPeriod(const std::string& s) {
    if (s.size() != 7 || s[4] != '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 4) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    int m = (s[5] - '0') * 10 + (s[6] - '0');
    return m >= 1 && m <= 12;
}

// draft → issued → paid；draft/issued 可作废。paid 与 void 是终态。
bool transitionOk(const std::string& cur, const std::string& next) {
    if (cur == next) return true;
    if (next == "issued") return cur == "draft";
    if (next == "paid") return cur == "issued";
    if (next == "void") return cur == "draft" || cur == "issued";
    return false;
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    out += "\"";
    return out;
}

json stmtJson(sqlite3_stmt* st) {
    json j;
    j["id"] = dbColInt(st, 0);
    j["tenant_id"] = dbColInt(st, 1);
    j["tenant_name"] = dbColText(st, 2);
    j["period"] = dbColText(st, 3);
    j["from_day"] = dbColText(st, 4);
    j["to_day"] = dbColText(st, 5);
    j["currency"] = dbColText(st, 6);
    j["total_micro"] = dbColInt(st, 7);
    j["status"] = dbColText(st, 8);
    json lines = json::parse(dbColText(st, 9), nullptr, false);
    j["lines"] = lines.is_discarded() ? json::array() : lines;
    j["note"] = dbColText(st, 10);
    j["created_by"] = dbColText(st, 11);
    j["created_ms"] = dbColInt(st, 12);
    j["updated_ms"] = dbColInt(st, 13);
    j["issued_ms"] = dbColInt(st, 14);
    j["paid_ms"] = dbColInt(st, 15);
    return j;
}

const char* kStmtSelect =
    "SELECT s.id,s.tenant_id,COALESCE(t.name,'未归属'),s.period,s.from_day,s.to_day,s.currency,"
    "s.total_micro,s.status,s.lines_json,s.note,s.created_by,s.created_ms,s.updated_ms,"
    "s.issued_ms,s.paid_ms FROM billing_statements s LEFT JOIN tenants t ON t.id=s.tenant_id";

}  // namespace

bool AdminDb::billingStatementGenerate(const std::string& period, long long tenant_id,
                                       const std::string& by, std::string& json_out,
                                       std::string& err) {
    if (!isPeriod(period)) { err = "账期格式应为 YYYY-MM"; return false; }
    std::string from = period + "-01";
    std::string to = period + "-31";

    // 先算出该账期的费用明细（沿用试算口径，不持锁调用，避免自锁）
    std::string sum_json;
    if (!billingSummary(from, to, tenant_id, sum_json, err)) return false;
    json sum = json::parse(sum_json, nullptr, false);
    if (sum.is_discarded() || !sum.is_object()) { err = "db error"; return false; }

    json created = json::array();
    json updated = json::array();
    json skipped = json::array();

    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = "db error"; return false; }
    for (auto& t : sum.value("tenants", json::array())) {
        long long tid = t.value("tenant_id", (long long)0);
        long long total = t.value("total_micro", (long long)0);
        json lines = t.value("items", json::array());
        if (tid <= 0 || lines.empty()) continue;

        long long sid = 0;
        std::string cur_status;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT id,status FROM billing_statements WHERE tenant_id=? AND period=?",
                    -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e);
                err = "db error";
                return false;
            }
            sqlite3_bind_int64(st, 1, tid);
            sqlite3_bind_text(st, 2, period.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                sid = sqlite3_column_int64(st, 0);
                cur_status = dbColText(st, 1);
            }
            sqlite3_finalize(st);
        }

        long long now = nowMs();
        if (sid == 0) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "INSERT INTO billing_statements(tenant_id,period,from_day,to_day,currency,"
                    "total_micro,status,lines_json,note,created_by,created_ms,updated_ms)"
                    " VALUES(?,?,?,?,'CNY',?,'draft',?,'',?,?,?)",
                    -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e);
                err = "db error";
                return false;
            }
            sqlite3_bind_int64(st, 1, tid);
            sqlite3_bind_text(st, 2, period.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, from.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, to.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 5, total);
            sqlite3_bind_text(st, 6, lines.dump().c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 7, by.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 8, now);
            sqlite3_bind_int64(st, 9, now);
            bool ok = sqlite3_step(st) == SQLITE_DONE;
            if (ok) sid = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(st);
            if (!ok) {
                exec("ROLLBACK", e);
                err = "db error";
                return false;
            }
            json add;
            add["id"] = sid;
            add["tenant_id"] = tid;
            add["total_micro"] = total;
            created.push_back(add);
            continue;
        }

        if (cur_status != "draft") {
            json sk;
            sk["id"] = sid;
            sk["tenant_id"] = tid;
            sk["status"] = cur_status;
            sk["reason"] = "已出账，不能再重算（先作废或手工调账）";
            skipped.push_back(sk);
            continue;
        }

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "UPDATE billing_statements SET from_day=?,to_day=?,total_micro=?,lines_json=?,"
                "updated_ms=? WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = "db error";
            return false;
        }
        sqlite3_bind_text(st, 1, from.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, to.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, total);
        sqlite3_bind_text(st, 4, lines.dump().c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, now);
        sqlite3_bind_int64(st, 6, sid);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            err = "db error";
            return false;
        }
        json up;
        up["id"] = sid;
        up["tenant_id"] = tid;
        up["total_micro"] = total;
        updated.push_back(up);
    }
    if (!exec("COMMIT", e)) {
        exec("ROLLBACK", e);
        err = "db error";
        return false;
    }

    json out;
    out["period"] = period;
    out["from"] = from;
    out["to"] = to;
    out["created"] = created;
    out["updated"] = updated;
    out["skipped"] = skipped;
    json_out = out.dump();
    return true;
}

bool AdminDb::billingStatementList(const std::string& period, long long tenant_id,
                                   const std::string& status, std::string& json_out) {
    std::string sql = std::string(kStmtSelect) + " WHERE 1=1";
    if (!period.empty()) sql += " AND s.period=?";
    if (tenant_id > 0) sql += " AND s.tenant_id=?";
    if (!status.empty()) sql += " AND s.status=?";
    sql += " ORDER BY s.period DESC,s.id DESC LIMIT 500";

    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (!period.empty()) sqlite3_bind_text(st, idx++, period.c_str(), -1, SQLITE_TRANSIENT);
    if (tenant_id > 0) sqlite3_bind_int64(st, idx++, tenant_id);
    if (!status.empty()) sqlite3_bind_text(st, idx++, status.c_str(), -1, SQLITE_TRANSIENT);
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) arr.push_back(stmtJson(st));
    sqlite3_finalize(st);
    json out;
    out["items"] = arr;
    json_out = out.dump();
    return true;
}

bool AdminDb::billingStatementGet(long long id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, (std::string(kStmtSelect) + " WHERE s.id=?").c_str(), -1, &st,
                           nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) json_out = stmtJson(st).dump();
    sqlite3_finalize(st);
    if (!found) { err = "结算单不存在"; return false; }
    return true;
}

bool AdminDb::billingStatementSetStatus(long long id, const std::string& status,
                                       const std::string& note, bool set_note,
                                       std::string& json_out, std::string& err) {
    if (status != "issued" && status != "paid" && status != "void") {
        err = "状态只能是 issued/paid/void";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::string cur;
        {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT status FROM billing_statements WHERE id=?", -1, &st,
                                   nullptr) != SQLITE_OK) {
                err = "db error";
                return false;
            }
            sqlite3_bind_int64(st, 1, id);
            bool found = sqlite3_step(st) == SQLITE_ROW;
            if (found) cur = dbColText(st, 0);
            sqlite3_finalize(st);
            if (!found) { err = "结算单不存在"; return false; }
        }
        if (!transitionOk(cur, status)) {
            err = "不能从 " + cur + " 变更为 " + status;
            return false;
        }

        long long now = nowMs();
        std::string sql = "UPDATE billing_statements SET status=?,updated_ms=?";
        if (set_note) sql += ",note=?";
        if (status == "issued") sql += ",issued_ms=?";
        if (status == "paid") sql += ",paid_ms=?";
        sql += " WHERE id=?";

        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
            err = "db error";
            return false;
        }
        int idx = 1;
        sqlite3_bind_text(st, idx++, status.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, idx++, now);
        if (set_note) sqlite3_bind_text(st, idx++, note.c_str(), -1, SQLITE_TRANSIENT);
        if (status == "issued" || status == "paid") sqlite3_bind_int64(st, idx++, now);
        sqlite3_bind_int64(st, idx++, id);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) err = sqlite3_errmsg(db_);
        sqlite3_finalize(st);
        if (!ok) return false;
    }

    json_out.clear();
    return billingStatementGet(id, json_out, err);
}

bool AdminDb::billingStatementCsv(long long id, std::string& csv_out, std::string& err) {
    std::string js;
    if (!billingStatementGet(id, js, err)) return false;
    json s = json::parse(js, nullptr, false);
    if (s.is_discarded()) { err = "db error"; return false; }

    // Excel 需要 BOM 才按 UTF-8 打开
    std::string out = "\xEF\xBB\xBF";
    out += "tenant_id,tenant_name,period,status,metric,qty,unit_size,price_micro,amount_micro,note\n";
    std::string tid = std::to_string(s.value("tenant_id", (long long)0));
    std::string tname = csvField(s.value("tenant_name", std::string()));
    std::string period = s.value("period", std::string());
    std::string status = s.value("status", std::string());
    std::string note = csvField(s.value("note", std::string()));
    for (auto& l : s.value("lines", json::array())) {
        out += tid + "," + tname + "," + period + "," + status + ",";
        out += csvField(l.value("metric", std::string())) + ",";
        out += std::to_string(l.value("qty", (long long)0)) + ",";
        out += std::to_string(l.value("unit_size", (long long)1)) + ",";
        out += std::to_string(l.value("price_micro", (long long)0)) + ",";
        out += std::to_string(l.value("amount_micro", (long long)0)) + ",";
        out += note + "\n";
    }
    csv_out = out;
    return true;
}
