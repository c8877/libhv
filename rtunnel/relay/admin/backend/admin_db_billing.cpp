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
